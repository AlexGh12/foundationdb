/*
 * DiskQueue.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbserver/IDiskQueue.h"
#include "fdbrpc/IAsyncFile.h"
#include "fdbserver/Knobs.h"
#include "fdbrpc/simulator.h"
#include "flow/genericactors.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

typedef bool(*compare_pages)(void*,void*);
typedef int64_t loc_t;

struct StringBuffer {
	Standalone<StringRef> str;
	int reserved;
	UID id;

	StringBuffer(UID fromFileID) : reserved(0), id( fromFileID ) {}

	int size() const { return str.size(); }
	StringRef& ref() { return str; }
	void clear() {
		str = Standalone<StringRef>();
		reserved = 0;
	}
	void clearReserve(int size) {
		str = Standalone<StringRef>();
		reserved = size;
		ref() = StringRef( new (str.arena()) uint8_t[size], 0 );
	}
	void append( StringRef x ) {
		memcpy( append(x.size()), x.begin(), x.size() );
	}
	void* append(int bytes) {
		ASSERT( str.size() + bytes <= reserved );
		void* p = const_cast<uint8_t*>(str.end());
		ref() = StringRef( str.begin(), str.size()+bytes );
		return p;
	}
	StringRef pop_front(int bytes) {
		ASSERT( bytes <= str.size() );
		StringRef result = str.substr(0, bytes);
		ref() = str.substr(bytes);
		return result;
	}
	void alignReserve(int alignment, int size) {
		ASSERT( alignment && (alignment & (alignment-1)) == 0 );  // alignment is a power of two

		if (size >= reserved) {
			// SOMEDAY: Use a new arena and discard the old one after copying?
			reserved = std::max( size, reserved*2 );
			if( reserved > 1e9 ) {
				printf("WOAH! Huge allocation\n");
				TraceEvent(SevError, "StringBufferHugeAllocation", id).detail("Alignment", alignment).detail("Reserved", reserved).backtrace();
			}
			uint8_t* b = new (str.arena()) uint8_t[reserved+alignment-1];
			uint8_t* e = b + (reserved+alignment-1);

			uint8_t* p = (uint8_t*)(int64_t(b+alignment-1) & ~(alignment-1));  // first multiple of alignment greater than or equal to b
			ASSERT( p>=b && p+reserved<=e && int64_t(p)%alignment == 0 );

			memcpy(p, str.begin(), str.size());
			ref() = StringRef( p, str.size() );
		}
	}
};

struct SyncQueue : ReferenceCounted<SyncQueue> {
	SyncQueue( int outstandingLimit, Reference<IAsyncFile> file )
		: outstandingLimit(outstandingLimit), file(file)
	{
		for(int i=0; i<outstandingLimit; i++)
			outstanding.push_back( Void() );
	}

	Future<Void> onSync() {  // Future is set when all writes completed before the call to onSync are complete
		if (outstanding.size() <= outstandingLimit)
			outstanding.push_back( waitAndSync(this) );
		return outstanding.back();
	}

private:
	int outstandingLimit;
	Deque<Future<Void>> outstanding;
	Reference<IAsyncFile> file;

	ACTOR static Future<Void> waitAndSync(SyncQueue* self) {
		wait( self->outstanding.front() );
		self->outstanding.pop_front();
		wait( self->file->sync() );
		return Void();
	}
};

// We use a Tracked instead of a Reference when the shutdown/destructor code would need to wait().
template <typename T>
class Tracked {
protected:
	struct TrackMe : NonCopyable {
		T* self;
		explicit TrackMe( T* self ) : self(self) {
			self->actorCount++;
			if (self->actorCount == 1) self->actorCountIsZero.set(false);
		}
		~TrackMe() {
			self->actorCount--;
			if (self->actorCount == 0) self->actorCountIsZero.set(true);
		}
	};

	Future<Void> onSafeToDestruct() {
		if (actorCountIsZero.get()) {
			return Void();
		} else {
			return actorCountIsZero.onChange();
		}
	}

private:
	int actorCount = 0;
	AsyncVar<bool> actorCountIsZero = true;
};

class RawDiskQueue_TwoFiles : public Tracked<RawDiskQueue_TwoFiles> {
public:
	RawDiskQueue_TwoFiles( std::string basename, std::string fileExtension, UID dbgid, int64_t fileSizeWarningLimit )
		: basename(basename), fileExtension(fileExtension), onError(delayed(error.getFuture())), onStopped(stopped.getFuture()),
		readingFile(-1), readingPage(-1), writingPos(-1), dbgid(dbgid),
		dbg_file0BeginSeq(0), fileExtensionBytes(10<<20), readingBuffer( dbgid ),
		readyToPush(Void()), fileSizeWarningLimit(fileSizeWarningLimit), lastCommit(Void()), isFirstCommit(true)
	{
		if(BUGGIFY)
			fileExtensionBytes = 8<<10;
		files[0].dbgFilename = filename(0);
		files[1].dbgFilename = filename(1);
		// We issue reads into firstPages, so it needs to be 4k aligned.
		firstPages.reserve(firstPages.arena(), 2);
		void* pageMemory = operator new (sizeof(Page) * 3, firstPages.arena());
		firstPages[0] = (Page*)((((uintptr_t)pageMemory + 4095) / 4096) * 4096);
		memset(firstPages[0], 0, sizeof(Page));
		firstPages[1] = (Page*)((uintptr_t)firstPages[0] + 4096);
		memset(firstPages[1], 0, sizeof(Page));
		stallCount.init(LiteralStringRef("RawDiskQueue.StallCount"));
	}

	Future<Void> pushAndCommit( StringRef pageData, StringBuffer* pageMem, uint64_t poppedPages ) {
		return pushAndCommit( this, pageData, pageMem, poppedPages );
	}

	void stall() {
		stallCount++;
		readyToPush = lastCommit;
	}

	Future<Standalone<StringRef>> readFirstAndLastPages( compare_pages compare ) { return readFirstAndLastPages(this,compare); }

	void setStartPage( int file, int64_t page ) {
		TraceEvent("RDQSetStart", dbgid).detail("FileNum",file).detail("PageNum",page).detail("File0Name", files[0].dbgFilename);
		readingFile = file;
		readingPage = page;
	}

	Future<Void> setPoppedPage( int file, int64_t page, int64_t debugSeq ) { return setPoppedPage(this, file, page, debugSeq); }

	// FIXME: let the caller pass in where to write the data.
	Future<Standalone<StringRef>> read(int file, int page, int nPages) { return read(this, file, page, nPages); }
	Future<Standalone<StringRef>> readNextPage() { return readNextPage(this); }
	Future<Void> truncateBeforeLastReadPage() { return truncateBeforeLastReadPage(this); }

	Future<Void> getError() { return onError; }
	Future<Void> onClosed() { return onStopped; }
	void dispose() { shutdown(this, true); }
	void close() { shutdown(this, false); }

	StorageBytes getStorageBytes() {
		int64_t free;
		int64_t total;

		g_network->getDiskBytes(parentDirectory(basename), free, total);

		return StorageBytes(free, total, files[0].size + files[1].size, free); // TODO: we could potentially do better in the available field by accounting for the unused pages at the end of the file
	}

//private:
	struct Page { uint8_t data[_PAGE_SIZE]; };

	struct File {
		Reference<IAsyncFile> f;
		int64_t size; // always a multiple of _PAGE_SIZE, even if the physical file isn't for some reason
		int64_t popped;
		std::string dbgFilename;
		Reference<SyncQueue> syncQueue;

		File() : size(-1), popped(-1) {}

		void setFile(Reference<IAsyncFile> f) {
			this->f = f;
			this->syncQueue = Reference<SyncQueue>( new SyncQueue(1, f) );
		}
	};
	File files[2];  // After readFirstAndLastPages(), files[0] is logically before files[1] (pushes are always into files[1])
	Standalone<VectorRef<Page*>> firstPages;

	std::string basename;
	std::string fileExtension;
	std::string filename(int i) const { return basename + format("%d.%s", i, fileExtension.c_str()); }

	UID dbgid;
	int64_t dbg_file0BeginSeq;
	int64_t fileSizeWarningLimit;

	Promise<Void> error, stopped;
	Future<Void> onError, onStopped;

	Future<Void> readyToPush;
	Future<Void> lastCommit;
	bool isFirstCommit;

	StringBuffer readingBuffer; // Pages that have been read and not yet returned
	int readingFile;  // i if the next page after readingBuffer should be read from files[i], 2 if recovery is complete
	int64_t readingPage;  // Page within readingFile that is the next page after readingBuffer

	int64_t writingPos;  // Position within files[1] that will be next written

	int64_t fileExtensionBytes;

	Int64MetricHandle stallCount;

	Future<Void> truncateFile(int file, int64_t pos) { return truncateFile(this, file, pos); }

	Future<Void> push(StringRef pageData, vector<Reference<SyncQueue>>& toSync) {
		// Write the given data to the queue files, swapping or extending them if necessary.
		// Don't do any syncs, but push the modified file(s) onto toSync.
		ASSERT( readingFile == 2 );
		ASSERT( pageData.size() % _PAGE_SIZE == 0 );
		ASSERT( int64_t(pageData.begin()) % _PAGE_SIZE == 0 );
		ASSERT( writingPos % _PAGE_SIZE == 0 );
		ASSERT( files[0].size % _PAGE_SIZE == 0 && files[1].size % _PAGE_SIZE == 0 );

		vector<Future<Void>> waitfor;

		if (pageData.size() + writingPos > files[1].size) {
			if ( files[0].popped == files[0].size ) {
				// Finish files[1] and swap
				int p = files[1].size - writingPos;
				if(p > 0) {
					toSync.push_back( files[1].syncQueue );
					/*TraceEvent("RDQWriteAndSwap", this->dbgid).detail("File1name", files[1].dbgFilename).detail("File1size", files[1].size)
						.detail("WritingPos", writingPos).detail("WritingBytes", p);*/
					waitfor.push_back( files[1].f->write( pageData.begin(), p, writingPos ) );
					pageData = pageData.substr( p );
				}

				dbg_file0BeginSeq += files[0].size;
				std::swap(files[0], files[1]);
				std::swap(firstPages[0], firstPages[1]);
				files[1].popped = 0;
				writingPos = 0;
			} else {
				// Extend files[1] to accomodate the new write and about 10MB or 2x current size for future writes.
				/*TraceEvent("RDQExtend", this->dbgid).detail("File1name", files[1].dbgFilename).detail("File1size", files[1].size)
					.detail("ExtensionBytes", fileExtensionBytes);*/
				int64_t minExtension = pageData.size() + writingPos - files[1].size;
				files[1].size += std::min(std::max(fileExtensionBytes, minExtension), files[0].size+files[1].size+minExtension);
				waitfor.push_back( files[1].f->truncate( files[1].size ) );

				if(fileSizeWarningLimit > 0 && files[1].size > fileSizeWarningLimit) {
					TraceEvent(SevWarnAlways, "DiskQueueFileTooLarge", dbgid).suppressFor(1.0).detail("Filename", filename(1)).detail("Size", files[1].size);
				}
			}
		}

		if (writingPos == 0) {
			*firstPages[1] = *(const Page*)pageData.begin();
		}

		/*TraceEvent("RDQWrite", this->dbgid).detail("File1name", files[1].dbgFilename).detail("File1size", files[1].size)
			.detail("WritingPos", writingPos).detail("WritingBytes", pageData.size());*/
		files[1].size = std::max( files[1].size, writingPos + pageData.size() );
		toSync.push_back( files[1].syncQueue );
		waitfor.push_back( files[1].f->write( pageData.begin(), pageData.size(), writingPos ) );
		writingPos += pageData.size();

		return waitForAll(waitfor);
	}

	ACTOR static UNCANCELLABLE Future<Void> pushAndCommit(RawDiskQueue_TwoFiles* self, StringRef pageData, StringBuffer* pageMem, uint64_t poppedPages) {
		state Promise<Void> pushing, committed;
		state Promise<Void> errorPromise = self->error;
		state std::string filename = self->files[0].dbgFilename;
		state UID dbgid = self->dbgid;
		state vector<Reference<SyncQueue>> syncFiles;
		state Future<Void> lastCommit = self->lastCommit;
		try {
			// pushing might need to wait for previous pushes to start (to maintain order) or for
			// a previous commit to finish if stall() was called
			Future<Void> ready = self->readyToPush;
			self->readyToPush = pushing.getFuture();
			self->lastCommit = committed.getFuture();

			// the first commit must complete before we can pipeline other commits so that we will always have a valid page to binary search to
			if(self->isFirstCommit) {
				self->isFirstCommit = false;
				self->readyToPush = self->lastCommit;
			}

			wait( ready );

			TEST( pageData.size() > sizeof(Page) ); // push more than one page of data

			Future<Void> pushed = self->push( pageData, syncFiles );
			pushing.send(Void());
			ASSERT( syncFiles.size() >= 1 && syncFiles.size() <= 2 );
			TEST(2==syncFiles.size());  // push spans both files
			wait( pushed );

			if (!g_network->isSimulated()) {
				delete pageMem;
			}
			pageMem = 0;

			Future<Void> sync = syncFiles[0]->onSync();
			for(int i=1; i<syncFiles.size(); i++) sync = sync && syncFiles[i]->onSync();
			wait( sync );
			wait( lastCommit );

			//Calling check_yield instead of yield to avoid a destruction ordering problem in simulation
			if(g_network->check_yield(g_network->getCurrentTask())) {
				wait(delay(0, g_network->getCurrentTask()));
			}

			self->updatePopped( poppedPages*sizeof(Page) );

			/*TraceEvent("RDQCommitEnd", self->dbgid).detail("DeltaPopped", poppedPages*sizeof(Page)).detail("PoppedCommitted", self->dbg_file0BeginSeq + self->files[0].popped + self->files[1].popped)
				.detail("File0Size", self->files[0].size).detail("File1Size", self->files[1].size)
				.detail("File0Name", self->files[0].dbgFilename).detail("SyncedFiles", syncFiles.size());*/

			committed.send(Void());
		} catch (Error& e) {
			if (!g_network->isSimulated()) {
				delete pageMem;
			}
			TEST(true);  // push error
			TEST(2==syncFiles.size());  // push spanning both files error
			TraceEvent(SevError, "RDQPushAndCommitError", dbgid).error(e, true).detail("InitialFilename0", filename);

			if (errorPromise.canBeSet()) errorPromise.sendError(e);
			if (pushing.canBeSet()) pushing.sendError(e);
			if (committed.canBeSet()) committed.sendError(e);

			throw e;
		}
		return Void();
	}

	void updatePopped( int64_t popped ) {
		int64_t pop0 = std::min(popped, files[0].size - files[0].popped);
		files[0].popped += pop0;
		files[1].popped += popped - pop0;
	}


	ACTOR static Future<Void> setPoppedPage( RawDiskQueue_TwoFiles *self, int file, int64_t page, int64_t debugSeq ) {
		self->files[file].popped = page*sizeof(Page);
		if (file) self->files[0].popped = self->files[0].size;
		else self->files[1].popped = 0;
		self->dbg_file0BeginSeq = debugSeq - self->files[1].popped - self->files[0].popped;

		//If we are starting in file 1, we truncate file 0 in case it has been corrupted.
		//  In particular, we are trying to avoid a dropped or corrupted write to the first page of file 0 causing it to be sequenced before file 1,
		//  when in fact it contains many pages that follow file 1.  These ok pages may be incorrectly read if the machine dies after overwritting the
		//  first page of file 0 and is then recovered
		if(file == 1)
			wait(self->truncateFile(self, 0, 0));

		return Void();
	}

	ACTOR static Future<Void> openFiles( RawDiskQueue_TwoFiles* self ) {
		state vector<Future<Reference<IAsyncFile>>> fs;
		for(int i=0; i<2; i++)
			fs.push_back( IAsyncFileSystem::filesystem()->open( self->filename(i), IAsyncFile::OPEN_READWRITE | IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_UNBUFFERED | IAsyncFile::OPEN_LOCK, 0 ) );
		wait( waitForAllReady(fs) );

		// Treatment of errors here is important.  If only one of the two files is present
		// (due to a power failure during creation or deletion, or administrative error) we don't want to
		// open the queue!

		if (!fs[0].isError() && !fs[1].isError()) {
			// Both files were opened OK: success
		} else if ( fs[0].isError() && fs[0].getError().code() == error_code_file_not_found &&
					fs[1].isError() && fs[1].getError().code() == error_code_file_not_found )
		{
			// Neither file was found: we can create a new queue
			// OPEN_ATOMIC_WRITE_AND_CREATE defers creation (using a .part file) until the calls to sync() below
			TraceEvent("DiskQueueCreate").detail("File0", self->filename(0));
			for(int i=0; i<2; i++)
				fs[i] = IAsyncFileSystem::filesystem()->open( self->filename(i), IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE | IAsyncFile::OPEN_CREATE | IAsyncFile::OPEN_READWRITE | IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_UNBUFFERED | IAsyncFile::OPEN_LOCK, 0600 );

			// Any error here is fatal
			wait( waitForAll(fs) );

			// sync on each file to actually create it will be done below
		} else {
			// One file had a more serious error or one file is present and the other is not.  Die.
			if (!fs[0].isError() || (fs[1].isError() && fs[1].getError().code() != error_code_file_not_found))
				throw fs[1].getError();
			else
				throw fs[0].getError();
		}

		// fsync both files.  This is necessary to trigger atomic file creation in the creation case above.
		// It also permits the recovery code to assume that whatever it reads is durable.  Otherwise a prior
		// process could have written (but not synchronized) data to the file which we will read but which
		// might not survive a reboot.  The recovery code assumes otherwise and could corrupt the disk.
		vector<Future<Void>> syncs;
		for(int i=0; i<fs.size(); i++)
			syncs.push_back( fs[i].get()->sync() );
		wait(waitForAll(syncs));

		// Successfully opened or created; fill in self->files[]
		for(int i=0; i<2; i++)
			self->files[i].setFile(fs[i].get());

		return Void();
	}

	ACTOR static void shutdown( RawDiskQueue_TwoFiles* self, bool deleteFiles ) {
		// Wait for all reads and writes on the file, and all actors referencing self, to be finished
		state Error error = success();
		try {
			wait(success(errorOr(self->lastCommit)));
			wait( self->onSafeToDestruct() );

			for(int i=0; i<2; i++)
				self->files[i].f.clear();

			if (deleteFiles) {
				TraceEvent("DiskQueueShutdownDeleting", self->dbgid)
					.detail("File0", self->filename(0))
					.detail("File1", self->filename(1));
				wait( IAsyncFileSystem::filesystem()->incrementalDeleteFile( self->filename(0), false ) );
				wait( IAsyncFileSystem::filesystem()->incrementalDeleteFile( self->filename(1), true ) );
			}
			TraceEvent("DiskQueueShutdownComplete", self->dbgid)
				.detail("DeleteFiles", deleteFiles)
				.detail("File0", self->filename(0));
		} catch( Error &e ) {
			TraceEvent(SevError, "DiskQueueShutdownError", self->dbgid)
				.error(e,true)
				.detail("Reason", e.code() == error_code_platform_error ? "could not delete database" : "unknown");
			error = e;
		}

		if( error.code() != error_code_actor_cancelled ) {
			if (self->stopped.canBeSet()) self->stopped.send(Void());
			if (self->error.canBeSet()) self->error.send(Never());
			delete self;
		}
	}

	ACTOR static UNCANCELLABLE Future<Standalone<StringRef>> readFirstAndLastPages(RawDiskQueue_TwoFiles* self, compare_pages compare) {
		state TrackMe trackMe(self);

		try {
			// Open both files or create both files
			wait( openFiles(self) );

			// Get the file sizes
			vector<Future<int64_t>> fsize;
			for(int i=0; i<2; i++)
				fsize.push_back( self->files[i].f->size() );
			vector<int64_t> file_sizes = wait( getAll(fsize) );
			for(int i=0; i<2; i++) {
				// SOMEDAY: If the file size is not a multiple of page size, it may never be shortened.  Change this?
				self->files[i].size = file_sizes[i] - file_sizes[i] % sizeof(Page);
				ASSERT( self->files[i].size % sizeof(Page) == 0 );
			}

			// Read the first pages
			vector<Future<int>> reads;
			for(int i=0; i<2; i++)
				if( self->files[i].size > 0)
					reads.push_back( self->files[i].f->read( self->firstPages[i], sizeof(Page), 0 ) );
			wait( waitForAll(reads) );

			// Determine which file comes first
			if ( compare( self->firstPages[1], self->firstPages[0] ) ) {
				std::swap( self->firstPages[0], self->firstPages[1] );
				std::swap( self->files[0], self->files[1] );
			}

			if ( !compare( self->firstPages[1], self->firstPages[1] ) ) {
				// Both files are invalid... the queue is empty!
				// Begin pushing at the beginning of files[1]

				//Truncate both files, since perhaps only the first pages are corrupted.  This avoids cases where overwritting the first page and then terminating makes
				//subsequent pages valid upon recovery.
				vector<Future<Void>> truncates;
				for(int i = 0; i < 2; ++i)
					if(self->files[i].size > 0)
						truncates.push_back(self->truncateFile(self, i, 0));

				wait(waitForAll(truncates));


				self->files[0].popped = self->files[0].size;
				self->files[1].popped = 0;
				self->writingPos = 0;
				self->readingFile = 2;
				return Standalone<StringRef>();
			}

			// A page in files[1] is "valid" iff compare(self->firstPages[1], page)
			// Binary search to find a page in files[1] that is "valid" but the next page is not valid
			// Invariant: the page at begin is valid, and the page at end is invalid
			state int64_t begin = 0;
			state int64_t end = self->files[1].size/sizeof(Page);
			state Standalone<StringRef> middlePageAllocation = makeAlignedString(sizeof(Page), sizeof(Page));
			state Page *middlePage = (Page*)middlePageAllocation.begin();
			while ( begin + 1 != end ) {
				state int64_t middle = (begin+end)/2;
				ASSERT( middle > begin && middle < end );  // So the loop always changes begin or end

				int len = wait( self->files[1].f->read( middlePage, sizeof(Page), middle*sizeof(Page) ) );
				ASSERT( len == sizeof(Page) );

				bool middleValid = compare( self->firstPages[1], middlePage );

				TraceEvent("RDQBS", self->dbgid).detail("Begin", begin).detail("End", end).detail("Middle", middle).detail("Valid", middleValid).detail("File0Name", self->files[0].dbgFilename);

				if (middleValid)
					begin = middle;
				else
					end = middle;
			}
			// Now by the invariant and the loop condition, begin is a valid page and begin+1 is an invalid page
			// Check that begin+1 is invalid
			int len1 = wait( self->files[1].f->read( middlePage, sizeof(Page), (begin+1)*sizeof(Page) ) );
			ASSERT( !(len1 == sizeof(Page) && compare( self->firstPages[1], middlePage )) );

			// Read it
			int len2 = wait( self->files[1].f->read( middlePage, sizeof(Page), begin*sizeof(Page) ) );
			ASSERT( len2 == sizeof(Page) && compare( self->firstPages[1], middlePage ) );

			TraceEvent("RDQEndFound", self->dbgid).detail("File0Name", self->files[0].dbgFilename).detail("Pos", begin).detail("FileSize", self->files[1].size);

			return middlePageAllocation;
		} catch (Error& e) {
			bool ok = e.code() == error_code_file_not_found;
			TraceEvent(ok ? SevInfo : SevError, "RDQReadFirstAndLastPagesError", self->dbgid).error(e, true).detail("File0Name", self->files[0].dbgFilename);
			if (!self->error.isSet()) self->error.sendError(e);
			throw;
		}
	}

	ACTOR static Future<Standalone<StringRef>> read(RawDiskQueue_TwoFiles* self, int file, int pageOffset, int nPages) {
		state TrackMe trackMe(self);
		state const size_t bytesRequested = nPages * sizeof(Page);
		state Standalone<StringRef> result = makeAlignedString(sizeof(Page), bytesRequested);
		if (file == 1) ASSERT_WE_THINK(pageOffset * sizeof(Page) + bytesRequested <= self->writingPos );
		int bytesRead = wait( self->files[file].f->read( mutateString(result), bytesRequested, pageOffset*sizeof(Page) ) );
		ASSERT_WE_THINK(bytesRead == bytesRequested);
		return result;
	}

	Future<int> fillReadingBuffer() {
		// If we're right at the end of a file...
		if ( readingPage*sizeof(Page) >= (size_t)files[readingFile].size ) {
			readingFile++;
			readingPage = 0;
			if (readingFile>=2) {
				// Recovery complete
				readingBuffer.clear();
				writingPos = files[1].size;
				return 0;
			}
		}

		// Read up to 1MB into readingBuffer
		int len = std::min<int64_t>( (files[readingFile].size/sizeof(Page) - readingPage)*sizeof(Page), BUGGIFY_WITH_PROB(1.0) ? sizeof(Page)*g_random->randomInt(1,4) : (1<<20) );
		readingBuffer.clear();
		readingBuffer.alignReserve( sizeof(Page), len );
		void* p = readingBuffer.append(len);

		auto pos = readingPage * sizeof(Page);
		readingPage += len / sizeof(Page);
		ASSERT( int64_t(p) % sizeof(Page) == 0 );
		return files[readingFile].f->read( p, len, pos );
	}

	ACTOR static UNCANCELLABLE Future<Standalone<StringRef>> readNextPage(RawDiskQueue_TwoFiles* self) {
		state TrackMe trackMe(self);

		try {
			ASSERT( self->readingFile < 2 );
			ASSERT( self->files[0].f && self->files[1].f );


			if (!self->readingBuffer.size()) {
				state Future<Void> f = Void();
				//if (BUGGIFY) f = delay( g_random->random01() * 0.1 );

				int read = wait( self->fillReadingBuffer() );
				ASSERT( read == self->readingBuffer.size() );

				wait(f);
			}
			if (!self->readingBuffer.size()) return Standalone<StringRef>();

			ASSERT( self->readingBuffer.size() >= sizeof(Page) );
			Standalone<StringRef> result = self->readingBuffer.pop_front( sizeof(Page) );
			return result;
		} catch (Error& e) {
			TEST(true);  // Read next page error
			TraceEvent(SevError, "RDQReadNextPageError", self->dbgid).error(e, true).detail("File0Name", self->files[0].dbgFilename);
			if (!self->error.isSet()) self->error.sendError(e);
			throw;
		}
	}

	ACTOR static UNCANCELLABLE Future<Void> truncateFile(RawDiskQueue_TwoFiles* self, int file, int64_t pos) {
		state TrackMe trackMe(self);
		TraceEvent("DQTruncateFile", self->dbgid).detail("File", file).detail("Pos", pos).detail("File0Name", self->files[0].dbgFilename);
		state Reference<IAsyncFile> f = self->files[file].f;  // Hold onto a reference in the off-chance that the DQ is removed from underneath us.
		if (pos == 0) {
			memset(self->firstPages[file], 0, _PAGE_SIZE);
		}
		wait( f->zeroRange( pos, self->files[file].size-pos ) );
		wait(self->files[file].syncQueue->onSync());
		// We intentionally don't return the f->zero future, so that TrackMe is destructed after f->zero finishes.
		return Void();
	}

	ACTOR static Future<Void> truncateBeforeLastReadPage( RawDiskQueue_TwoFiles* self ) {
		try {
			state int file = self->readingFile;
			state int64_t pos = (self->readingPage - self->readingBuffer.size()/sizeof(Page) - 1) * sizeof(Page);
			state vector<Future<Void>> commits;
			state bool swap = file==0;

			TEST( file==0 ); // truncate before last read page on file 0
			TEST( file==1 && pos != self->files[1].size ); // truncate before last read page on file 1

			self->readingFile = 2;
			self->readingBuffer.clear();
			self->writingPos = pos;

			while (file < 2) {
				commits.push_back(self->truncateFile(self, file, pos));
				file++;
				pos = 0;
			}

			wait( waitForAll(commits) );

			if (swap) {
				std::swap(self->files[0], self->files[1]);
				std::swap(self->firstPages[0], self->firstPages[1]);
				self->files[0].popped = self->files[0].size;
			}

			return Void();
		} catch (Error& e) {
			TraceEvent(SevError, "RDQTruncateBeforeLastReadPageError", self->dbgid).error(e).detail("File0Name", self->files[0].dbgFilename);
			if (!self->error.isSet()) self->error.sendError(e);
			throw;
		}
	}
};

class DiskQueue : public IDiskQueue, public Tracked<DiskQueue> {
public:
	// FIXME: Is setting lastCommittedSeq to -1 instead of 0 necessary?
	DiskQueue( std::string basename, std::string fileExtension, UID dbgid, int64_t fileSizeWarningLimit )
		: rawQueue( new RawDiskQueue_TwoFiles(basename, fileExtension, dbgid, fileSizeWarningLimit) ), dbgid(dbgid), anyPopped(false), nextPageSeq(0), poppedSeq(0), lastPoppedSeq(0),
		  nextReadLocation(-1), readBufPage(NULL), readBufPos(0), pushed_page_buffer(NULL), recovered(false), initialized(false), lastCommittedSeq(-1), warnAlwaysForMemory(true)
	{
	}

	virtual location push( StringRef contents ) {
		ASSERT( recovered );
		uint8_t const* begin = contents.begin();
		uint8_t const* end = contents.end();
		TEST( contents.size() && pushedPageCount() );  // More than one push between commits
		TEST( contents.size()>=4 && pushedPageCount() && backPage().remainingCapacity()<4 );  // Push right at the end of a page, possibly splitting size
		while (begin != end) {
			if (!pushedPageCount() || !backPage().remainingCapacity()) addEmptyPage();

			auto &p = backPage();
			int s = std::min<int>( p.remainingCapacity(), end-begin );
			memcpy( p.payload + p.payloadSize, begin, s );
			p.payloadSize += s;
			begin += s;
		}
		return endLocation();
	}

	virtual void pop( location upTo ) {
		ASSERT( !upTo.hi );
		ASSERT( !recovered || upTo.lo <= endLocation() );

		// The following ASSERT is NOT part of the intended contract of IDiskQueue, but alerts the user to a known bug where popping
		//  into uncommitted pages can cause a durability failure.
		// FIXME: Remove this ASSERT when popping into uncommitted pages is fixed
		if( upTo.lo > lastCommittedSeq ) {
			TraceEvent(SevError, "DQPopUncommittedData", dbgid)
				.detail("UpTo", upTo)
				.detail("LastCommittedSeq", lastCommittedSeq)
				.detail("File0Name", rawQueue->files[0].dbgFilename);
		}
		if (upTo.lo > poppedSeq) {
			poppedSeq = upTo.lo;
			anyPopped = true;
		}
	}

	virtual Future<Standalone<StringRef>> read(location from, location to) { return read(this, from, to); }

	int getMaxPayload() {
		return Page::maxPayload;
	}

	virtual int getCommitOverhead() {
		if(!pushedPageCount()) {
			if(!anyPopped)
				return 0;

			return Page::maxPayload;
		}
		else
			return backPage().remainingCapacity();
	}

	virtual Future<Void> commit() {
		ASSERT( recovered );
		if (!pushedPageCount()) {
			if (!anyPopped) return Void();
			addEmptyPage();
		}
		anyPopped = false;
		backPage().popped = poppedSeq;
		backPage().zeroPad();
		backPage().updateHash();

		if( pushedPageCount() >= 8000 ) {
			TraceEvent( warnAlwaysForMemory ? SevWarnAlways : SevWarn, "DiskQueueMemoryWarning", dbgid)
				.suppressFor(1.0)
				.detail("PushedPages", pushedPageCount())
				.detail("NextPageSeq", nextPageSeq)
				.detail("Details", format("%d pages", pushedPageCount()))
				.detail("File0Name", rawQueue->files[0].dbgFilename);
			if(g_network->isSimulated())
				warnAlwaysForMemory = false;
		}

		/*TraceEvent("DQCommit", dbgid).detail("Pages", pushedPageCount()).detail("LastPoppedSeq", lastPoppedSeq).detail("PoppedSeq", poppedSeq).detail("NextPageSeq", nextPageSeq)
			.detail("RawFile0Size", rawQueue->files[0].size).detail("RawFile1Size", rawQueue->files[1].size).detail("WritingPos", rawQueue->writingPos)
			.detail("RawFile0Name", rawQueue->files[0].dbgFilename);*/

		lastCommittedSeq = backPage().endSeq();
		auto f = rawQueue->pushAndCommit( pushed_page_buffer->ref(), pushed_page_buffer, poppedSeq/sizeof(Page) - lastPoppedSeq/sizeof(Page) );
		if (g_network->isSimulated()) {
			verifyCommit(this, f, pushed_page_buffer, ((Page*)pushed_page_buffer->ref().begin())->seq, lastCommittedSeq);
		}
		lastPoppedSeq = poppedSeq;
		pushed_page_buffer = 0;
		return f;
	}

	void stall() {
		rawQueue->stall();
	}

	virtual Future<bool> initializeRecovery() { return initializeRecovery( this ); }
	virtual Future<Standalone<StringRef>> readNext( int bytes ) { return readNext(this, bytes); }

	// FIXME: getNextReadLocation should ASSERT( initialized ), but the memory storage engine needs
	// to be changed to understand the new intiailizeRecovery protocol.
	virtual location getNextReadLocation() { return nextReadLocation; }

	virtual Future<Void> getError() { return rawQueue->getError(); }
	virtual Future<Void> onClosed() { return rawQueue->onClosed(); }

	virtual void dispose() {
		TraceEvent("DQDestroy", dbgid).detail("LastPoppedSeq", lastPoppedSeq).detail("PoppedSeq", poppedSeq).detail("NextPageSeq", nextPageSeq).detail("File0Name", rawQueue->files[0].dbgFilename);
		dispose(this);
	}
	ACTOR static void dispose(DiskQueue* self) {
		wait( self->onSafeToDestruct() );
		TraceEvent("DQDestroyDone", self->dbgid).detail("File0Name", self->rawQueue->files[0].dbgFilename);
		self->rawQueue->dispose();
		delete self;
	}

	virtual void close() {
		TraceEvent("DQClose", dbgid)
			.detail("LastPoppedSeq", lastPoppedSeq)
			.detail("PoppedSeq", poppedSeq)
			.detail("NextPageSeq", nextPageSeq)
			.detail("PoppedCommitted", rawQueue->dbg_file0BeginSeq + rawQueue->files[0].popped + rawQueue->files[1].popped)
			.detail("File0Name", rawQueue->files[0].dbgFilename);
		close(this);
	}
	ACTOR static void close(DiskQueue* self) {
		wait( self->onSafeToDestruct() );
		TraceEvent("DQCloseDone", self->dbgid).detail("File0Name", self->rawQueue->files[0].dbgFilename);
		self->rawQueue->close();
		delete self;
	}

	virtual StorageBytes getStorageBytes() {
		return rawQueue->getStorageBytes();
	}

private:
	#pragma pack(push, 1)
	struct PageHeader {
		UID hash;
		uint64_t seq;
		uint64_t popped;
		int payloadSize;
	};
	// The on disk format depends on the size of PageHeader.
	static_assert( sizeof(PageHeader) == 36, "PageHeader must be packed" );

	struct Page : PageHeader {
		static const int maxPayload = _PAGE_SIZE - sizeof(PageHeader);
		uint8_t payload[maxPayload];

		int remainingCapacity() const { return maxPayload - payloadSize; }
		uint64_t endSeq() const { return seq + sizeof(PageHeader) + payloadSize; }
		void updateHash() {
			// SOMEDAY: Better hash?
			uint32_t part[2] = { 0x12345678, 0xbeefabcd };
			hashlittle2( &seq, sizeof(Page)-sizeof(hash), &part[0], &part[1] );
			hash = UID( (int64_t(part[0])<<32)+part[1], 0xfdb );
		}
		bool checkHash() {
			UID h = hash;
			updateHash();
			if (h != hash) { std::swap(h, hash); return false; }
			return true;
		}
		void zeroPad() {
			memset( payload+payloadSize, 0, maxPayload-payloadSize );
		}
	};
	#pragma pack(pop)

	loc_t endLocation() const { return pushedPageCount() ? backPage().endSeq() : nextPageSeq; }

	void addEmptyPage() {
		if (pushedPageCount()) {
			backPage().updateHash();
			ASSERT( backPage().payloadSize == Page::maxPayload );
		}

		//pushed_pages.resize( pushed_pages.arena(), pushed_pages.size()+1 );
		if (!pushed_page_buffer) pushed_page_buffer = new StringBuffer( dbgid );
		pushed_page_buffer->alignReserve( sizeof(Page), pushed_page_buffer->size() + sizeof(Page) );
		pushed_page_buffer->append( sizeof(Page) );

		ASSERT( nextPageSeq%sizeof(Page)==0 );

		auto& p = backPage();
		memset(&p, 0, sizeof(Page)); // FIXME: unnecessary?
		p.payloadSize = 0;
		p.seq = nextPageSeq;
		nextPageSeq += sizeof(Page);
		p.popped = poppedSeq;

		if (pushedPageCount() == 8000) {
			TraceEvent("DiskQueueHighPageCount", dbgid)
				.detail("PushedPages", pushedPageCount())
				.detail("NextPageSeq", nextPageSeq)
				.detail("File0Name", rawQueue->files[0].dbgFilename);
		}
	}

	ACTOR static void verifyCommit(DiskQueue* self, Future<Void> commitSynced, StringBuffer* buffer, loc_t start, loc_t end) {
		state TrackMe trackme(self);
		try {
			wait( commitSynced );
			Standalone<StringRef> pagedData = wait( readPages(self, start, end) );
			const int startOffset = start % _PAGE_SIZE;
			const int dataLen = end - start;
			ASSERT( pagedData.substr(startOffset, dataLen).compare( buffer->ref().substr(0, dataLen) ) == 0 );
		} catch (Error& e) {
			if (e.code() != error_code_io_error) {
				delete buffer;
				throw;
			}
		}
		delete buffer;
	}

	ACTOR static Future<Standalone<StringRef>> readPages(DiskQueue *self, location start, location end) {
		state TrackMe trackme(self);
		state int fromFile;
		state int toFile;
		state int64_t fromPage;
		state int64_t toPage;
		state uint64_t file0size = self->firstPages(1).seq - self->firstPages(0).seq;
		ASSERT(end > start);
		ASSERT(start.lo >= self->firstPages(0).seq);
		self->findPhysicalLocation(start.lo, &fromFile, &fromPage, nullptr);
		self->findPhysicalLocation(end.lo-1, &toFile, &toPage, nullptr);
		if (fromFile == 0) { ASSERT( fromPage < file0size / _PAGE_SIZE ); }
		if (toFile == 0) { ASSERT( toPage < file0size / _PAGE_SIZE ); }
		// FIXME I think there's something with nextReadLocation we can do here when initialized && !recovered.
		if (fromFile == 1 && self->recovered) { ASSERT( fromPage < self->rawQueue->writingPos / _PAGE_SIZE ); }
		if (toFile == 1 && self->recovered) { ASSERT( toPage < self->rawQueue->writingPos / _PAGE_SIZE ); }
		if (fromFile == toFile) {
			ASSERT(toPage >= fromPage);
			Standalone<StringRef> pagedData = wait( self->rawQueue->read( fromFile, fromPage, toPage - fromPage + 1 ) );
			if ( self->firstPages(0).seq > start.lo ) {
				// Simulation allows for reads to be delayed and executed after overlapping subsequent
				// write operations.  This means that by the time our read was executed, it's possible
				// that both disk queue files have been completely overwritten.
				// I'm not clear what is the actual contract for read/write in this case, so simulation
				// might be a bit overly aggressive here, but it's behavior we need to tolerate.
				throw io_error();
			}
			ASSERT( ((Page*)pagedData.begin())->seq == start.lo / _PAGE_SIZE * _PAGE_SIZE );
			ASSERT(pagedData.size() == (toPage - fromPage + 1) * _PAGE_SIZE );

			ASSERT( ((Page*)pagedData.end() - 1)->seq == (end.lo - 1) / _PAGE_SIZE * _PAGE_SIZE );
			return pagedData;
		} else {
			ASSERT(fromFile == 0);
			state Standalone<StringRef> firstChunk;
			state Standalone<StringRef> secondChunk;
			wait( store(firstChunk, self->rawQueue->read( fromFile, fromPage, ( file0size / sizeof(Page) ) - fromPage )) &&
			      store(secondChunk, self->rawQueue->read( toFile, 0, toPage + 1 )) );
			if ( self->firstPages(0).seq > start.lo ) {
				// See above.
				throw io_error();
			}
			ASSERT(firstChunk.size() == ( ( file0size / sizeof(Page) ) - fromPage ) * _PAGE_SIZE );
			ASSERT( ((Page*)firstChunk.begin())->seq == start.lo / _PAGE_SIZE * _PAGE_SIZE );
			ASSERT(secondChunk.size() == (toPage + 1) * _PAGE_SIZE);
			ASSERT( ((Page*)secondChunk.end() - 1)->seq == (end.lo - 1) / _PAGE_SIZE * _PAGE_SIZE );
			return firstChunk.withSuffix(secondChunk);
		}
	}

	ACTOR static Future<Standalone<StringRef>> read(DiskQueue *self, location start, location end) {
		// This `state` is unnecessary, but works around pagedData wrongly becoming const
		// due to the actor compiler.
		state Standalone<StringRef> pagedData = wait(readPages(self, start, end));
		ASSERT(start.lo % sizeof(Page) == 0 ||
		       start.lo % sizeof(Page) >= sizeof(PageHeader));
		int startingOffset = start.lo % sizeof(Page);
		if (startingOffset > 0) startingOffset -= sizeof(PageHeader);
		ASSERT(end.lo % sizeof(Page) == 0 ||
		       end.lo % sizeof(Page) > sizeof(PageHeader));
		int endingOffset = end.lo % sizeof(Page);
		if (endingOffset == 0) endingOffset = sizeof(Page);
		if (endingOffset > 0) endingOffset -= sizeof(PageHeader);

		if ((end.lo-1)/sizeof(Page)*sizeof(Page) == start.lo/sizeof(Page)*sizeof(Page)) {
			// start and end are on the same page
			ASSERT(pagedData.size() == sizeof(Page));
			pagedData.contents() = pagedData.substr(sizeof(PageHeader) + startingOffset, endingOffset - startingOffset);
			return pagedData;
		} else {
			// FIXME: This allocation is excessive and unnecessary.  We know the overhead per page that
			// we'll be stripping out (sizeof(PageHeader)), so we should be able to do a smaller
			// allocation.  But we should be able to re-use the space allocated for pagedData, which
			// would mean not having to allocate 2x the space for a read.
			Standalone<StringRef> unpagedData = makeString(pagedData.size());
			uint8_t *buf = mutateString(unpagedData);
			memset(buf, 0, unpagedData.size());
			const Page *data = reinterpret_cast<const Page*>(pagedData.begin());

			// Only start copying from `start` in the first page.
			if( data->payloadSize > startingOffset ) {
				memcpy(buf, data->payload+startingOffset, data->payloadSize-startingOffset);
				buf += data->payloadSize-startingOffset;
			}
			data++;

			// Copy all the middle pages
			while (data->seq != ((end.lo-1)/sizeof(Page)*sizeof(Page))) {
				// These pages can have varying amounts of data, as pages with partial
				// data will be zero-filled when commit is called.
				memcpy(buf, data->payload, data->payloadSize);
				buf += data->payloadSize;
				data++;
			}

			// Copy only until `end` in the last page.
			memcpy(buf, data->payload, std::min(endingOffset, data->payloadSize));
			buf += std::min(endingOffset, data->payloadSize);

			unpagedData.contents() = unpagedData.substr(0, buf - unpagedData.begin());
			return unpagedData;
		}
	}

	void readFromBuffer( StringBuffer* result, int* bytes ) {
		// extract up to bytes from readBufPage into result
		int len = std::min( readBufPage->payloadSize - readBufPos, *bytes );
		if (len<=0) return;

		result->append( StringRef(readBufPage->payload+readBufPos, len) );

		readBufPos += len;
		*bytes -= len;
		nextReadLocation += len;
	}

	ACTOR static Future<Standalone<StringRef>> readNext( DiskQueue *self, int bytes ) {
		state StringBuffer result( self->dbgid );
		ASSERT(bytes >= 0);
		result.clearReserve(bytes);

		ASSERT( !self->recovered );

		if (!self->initialized) {
			bool recoveryComplete = wait( initializeRecovery(self) );

			if (recoveryComplete) {
				ASSERT( self->poppedSeq <= self->endLocation() );

				return Standalone<StringRef>();
			}
		}

		loop {
			if (self->readBufPage) {
				self->readFromBuffer( &result, &bytes );
				// if done, return
				if (!bytes) return result.str;
				ASSERT( self->readBufPos == self->readBufPage->payloadSize );
				self->readBufPage = 0;
				self->nextReadLocation += sizeof(Page) - self->readBufPos;
				self->readBufPos = 0;
			}

			Standalone<StringRef> page = wait( self->rawQueue->readNextPage() );
			if (!page.size()) {
				TraceEvent("DQRecEOF", self->dbgid).detail("NextReadLocation", self->nextReadLocation).detail("File0Name", self->rawQueue->files[0].dbgFilename);
				break;
			}
			ASSERT( page.size() == sizeof(Page) );

			self->readBufArena = page.arena();
			self->readBufPage = (Page*)page.begin();
			if (!self->readBufPage->checkHash() || self->readBufPage->seq < self->nextReadLocation/sizeof(Page)*sizeof(Page)) {
				TraceEvent("DQRecInvalidPage", self->dbgid).detail("NextReadLocation", self->nextReadLocation).detail("HashCheck", self->readBufPage->checkHash())
					.detail("Seq", self->readBufPage->seq).detail("Expect", self->nextReadLocation/sizeof(Page)*sizeof(Page)).detail("File0Name", self->rawQueue->files[0].dbgFilename);
				wait( self->rawQueue->truncateBeforeLastReadPage() );
				break;
			}
			//TraceEvent("DQRecPage", self->dbgid).detail("NextReadLoc", self->nextReadLocation).detail("Seq", self->readBufPage->seq).detail("Pop", self->readBufPage->popped).detail("Payload", self->readBufPage->payloadSize).detail("File0Name", self->rawQueue->files[0].dbgFilename);
			ASSERT( self->readBufPage->seq == self->nextReadLocation/sizeof(Page)*sizeof(Page) );
			self->lastPoppedSeq = self->readBufPage->popped;
		}

		// Recovery complete.
		// The fully durable popped point is self->lastPoppedSeq; tell the raw queue that.
		int f; int64_t p;
		TEST( self->lastPoppedSeq/sizeof(Page) != self->poppedSeq/sizeof(Page) );  // DiskQueue: Recovery popped position not fully durable
		self->findPhysicalLocation( self->lastPoppedSeq, &f, &p, "lastPoppedSeq" );
		wait(self->rawQueue->setPoppedPage( f, p, self->lastPoppedSeq/sizeof(Page)*sizeof(Page) ));

		// Writes go at the end of our reads (but on the next page)
		self->nextPageSeq = self->nextReadLocation/sizeof(Page)*sizeof(Page);
		if (self->nextReadLocation % sizeof(Page) > sizeof(PageHeader)) self->nextPageSeq += sizeof(Page);

		TraceEvent("DQRecovered", self->dbgid).detail("LastPoppedSeq", self->lastPoppedSeq).detail("PoppedSeq", self->poppedSeq).detail("NextPageSeq", self->nextPageSeq).detail("File0Name", self->rawQueue->files[0].dbgFilename);
		self->recovered = true;
		ASSERT( self->poppedSeq <= self->endLocation() );

		TEST( result.size() == 0 );  // End of queue at border between reads
		TEST( result.size() != 0 );  // Partial read at end of queue

		//The next read location isn't necessarily the end of the last commit, but this is sufficient for helping us check an ASSERTion
		self->lastCommittedSeq = self->nextReadLocation;

		return result.str;
	}

	ACTOR static Future<bool> initializeRecovery( DiskQueue* self ) {
		if (self->initialized) {
			return self->recovered;
		}
		Standalone<StringRef> lastPageData = wait( self->rawQueue->readFirstAndLastPages( &comparePages ) );
		self->initialized = true;

		if (!lastPageData.size()) {
			// There are no valid pages, so apparently this is a completely empty queue
			self->nextReadLocation = 0;
			self->lastCommittedSeq = 0;
			self->recovered = true;
			return true;
		}

		Page* lastPage = (Page*)lastPageData.begin();
		self->nextReadLocation = self->poppedSeq = lastPage->popped;

		/*
		state std::auto_ptr<Page> testPage(new Page);
		state int fileNum;
		for( fileNum=0; fileNum<2; fileNum++) {
			state int sizeNum;
			for( sizeNum=0; sizeNum < self->rawQueue->files[fileNum].size; sizeNum += sizeof(Page) ) {
				wait(success( self->rawQueue->files[fileNum].f->read( testPage.get(), sizeof(Page), sizeNum ) ));
				TraceEvent("PageData").detail("File", self->rawQueue->files[fileNum].dbgFilename).detail("SizeNum", sizeNum).detail("Seq", testPage->seq).detail("Hash", testPage->checkHash()).detail("Popped", testPage->popped);
			}
		}
		*/

		int file; int64_t page;
		self->findPhysicalLocation( self->poppedSeq, &file, &page, "poppedSeq" );
		self->rawQueue->setStartPage( file, page );

		self->readBufPos = self->nextReadLocation % sizeof(Page) - sizeof(PageHeader);
		if (self->readBufPos < 0) { self->nextReadLocation -= self->readBufPos; self->readBufPos = 0; }
		TraceEvent("DQRecStart", self->dbgid).detail("ReadBufPos", self->readBufPos).detail("NextReadLoc", self->nextReadLocation).detail("File0Name", self->rawQueue->files[0].dbgFilename);

		return false;
	}

	Page& firstPages(int i) {
		ASSERT( initialized );
		return *(Page*)rawQueue->firstPages[i];
	}

	void findPhysicalLocation( loc_t loc, int* file, int64_t* page, const char* context ) {
		bool ok = false;

		if (context)
			TraceEvent(SevInfo, "FindPhysicalLocation", dbgid)
				.detail("Page0Valid", firstPages(0).checkHash())
				.detail("Page0Seq", firstPages(0).seq)
				.detail("Page1Valid", firstPages(1).checkHash())
				.detail("Page1Seq", firstPages(1).seq)
				.detail("Location", loc)
				.detail("Context", context)
				.detail("File0Name", rawQueue->files[0].dbgFilename);

		for(int i = 1; i >= 0; i--)
			// FIXME: Removed `firstPages(i).checkHash() &&`
			// Only put a page in firstPages() if its checksum is valid
			if ( firstPages(i).seq <= (size_t)loc ) {
				*file = i;
				*page = (loc - firstPages(i).seq)/sizeof(Page);
				if (context)
					TraceEvent("FoundPhysicalLocation", dbgid)
						.detail("PageIndex", i)
						.detail("PageLocation", *page)
						.detail("SizeofPage", sizeof(Page))
						.detail("PageSequence", firstPages(i).seq)
						.detail("Location", loc)
						.detail("Context", context)
						.detail("File0Name", rawQueue->files[0].dbgFilename);
				ok = true;
				break;
			}
		if (!ok)
			TraceEvent(SevError, "DiskQueueLocationError", dbgid)
				.detail("Page0Valid", firstPages(0).checkHash())
				.detail("Page0Seq", firstPages(0).seq)
				.detail("Page1Valid", firstPages(1).checkHash())
				.detail("Page1Seq", firstPages(1).seq)
				.detail("Location", loc)
				.detail("Context", context ? context : "")
				.detail("File0Name", rawQueue->files[0].dbgFilename);
		ASSERT( ok );
	}

	// isValid(firstPage) == compare(firstPage, firstPage)
	// isValid(otherPage) == compare(firstPage, otherPage)
	// Swap file1, file2 if comparePages( file2.firstPage, file1.firstPage )
	static bool comparePages( void* v1, void* v2 ) {
		Page* p1 = (Page*)v1; Page* p2 = (Page*)v2;
		return p2->checkHash() && (p2->seq >= p1->seq || !p1->checkHash());
	}

	RawDiskQueue_TwoFiles *rawQueue;
	UID dbgid;

	bool anyPopped;  // pop() has been called since the most recent call to commit()
	bool warnAlwaysForMemory;
	loc_t nextPageSeq, poppedSeq;
	loc_t lastPoppedSeq;  // poppedSeq the last time commit was called
	loc_t lastCommittedSeq;

	// Buffer of pushed pages that haven't been committed.  The last one (backPage()) is still mutable.
	StringBuffer* pushed_page_buffer;
	Page& backPage() {
		ASSERT( pushedPageCount() );
		return ((Page*)pushed_page_buffer->ref().end())[-1];
	}
	Page const& backPage() const { return ((Page*)pushed_page_buffer->ref().end())[-1]; }
	int pushedPageCount() const { return pushed_page_buffer ? pushed_page_buffer->size() / sizeof(Page) : 0; }

	// Recovery state
	bool recovered;
	bool initialized;
	loc_t nextReadLocation;
	Arena readBufArena;
	Page* readBufPage;
	int readBufPos;
};

//A class wrapping DiskQueue which durably allows uncommitted data to be popped
//This works by performing two commits when uncommitted data is popped:
//	Commit 1 - pop only previously committed data and push new data
//  Commit 2 - finish pop into uncommitted data
class DiskQueue_PopUncommitted : public IDiskQueue {

public:
	DiskQueue_PopUncommitted( std::string basename, std::string fileExtension, UID dbgid, int64_t fileSizeWarningLimit ) : queue(new DiskQueue(basename, fileExtension, dbgid, fileSizeWarningLimit)), pushed(0), popped(0), committed(0) { };

	//IClosable
	Future<Void> getError() { return queue->getError(); }
	Future<Void> onClosed() { return queue->onClosed(); }
	void dispose() { queue->dispose(); delete this; }
	void close() { queue->close(); delete this; }

	//IDiskQueue
	Future<bool> initializeRecovery() { return queue->initializeRecovery(); }
	Future<Standalone<StringRef>> readNext( int bytes ) { return readNext(this, bytes); }

	virtual location getNextReadLocation() { return queue->getNextReadLocation(); }

	virtual Future<Standalone<StringRef>> read( location start, location end ) { return queue->read( start, end ); }

	virtual location push( StringRef contents ) {
		pushed = queue->push(contents);
		return pushed;
	}

	virtual void pop( location upTo ) {
		popped = std::max(popped, upTo);
		ASSERT_WE_THINK(committed >= popped);
		queue->pop(std::min(committed, popped));
	}

	virtual int getCommitOverhead() {
		return queue->getCommitOverhead() + (popped > committed ? queue->getMaxPayload() : 0);
	}

	Future<Void> commit() {
		location pushLocation = pushed;
		location popLocation = popped;

		Future<Void> commitFuture = queue->commit();

		bool updatePop = popLocation > committed;
		committed = pushLocation;

		if(updatePop) {
			ASSERT_WE_THINK(false);
			ASSERT(popLocation <= committed);

			queue->stall();   // Don't permit this pipelined commit to write anything to disk until the previous commit is totally finished
			pop(popLocation);
			commitFuture = commitFuture && queue->commit();
		}
		else
			TEST(true); //No uncommitted data was popped

		return commitFuture;
	}

	virtual StorageBytes getStorageBytes() { return queue->getStorageBytes(); }

private:
	DiskQueue *queue;
	location pushed;
	location popped;
	location committed;

	ACTOR static Future<Standalone<StringRef>> readNext( DiskQueue_PopUncommitted *self, int bytes ) {
		Standalone<StringRef> str = wait(self->queue->readNext(bytes));
		if(str.size() < bytes)
			self->pushed = self->getNextReadLocation();

		return str;
	}
};

IDiskQueue* openDiskQueue( std::string basename, std::string ext, UID dbgid, int64_t fileSizeWarningLimit ) {
	return new DiskQueue_PopUncommitted( basename, ext, dbgid, fileSizeWarningLimit );
}
