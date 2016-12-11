
#include <string.h>
#include <errno.h>
#include <sys/auxv.h>

// for dup2()
#include <unistd.h>
// for open()
#include <fcntl.h>
// for tcgetattr()
#include <termios.h>
// for stat()
#include <sys/stat.h>

#include <mlibc/ensure.h>
#include <mlibc/cxx-support.hpp>
#include <mlibc/frigg-alloc.hpp>
#include <mlibc/posix-pipe.hpp>

#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/string.hpp>
#include <frigg/protobuf.hpp>

#include <posix.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

struct Queue {
	Queue()
	: _queue(nullptr), _progress(0) { }

	HelQueue *getQueue() {
		if(!_queue) {
			auto ptr = getAllocator().allocate(sizeof(HelQueue) + 4096);
			_queue = reinterpret_cast<HelQueue *>(ptr);
			_queue->elementLimit = 128;
			_queue->queueLength = 4096;
			_queue->kernelState = 0;
			_queue->userState = 0;
		}
		return _queue;
	}

	void *dequeueSingle() {
		assert(_queue);

		auto e = __atomic_load_n(&_queue->kernelState, __ATOMIC_ACQUIRE);
		while(true) {
			assert(!(e & kHelQueueWantNext));

			if(_progress != (e & kHelQueueTail)) {
				assert(_progress < (e & kHelQueueTail));

				auto ptr = (char *)_queue + sizeof(HelQueue) + _progress;
				auto elem = reinterpret_cast<HelElement *>(ptr);
				_progress += sizeof(HelElement) + elem->length;
				return ptr + sizeof(HelElement);
			}

			if(!(e & kHelQueueWaiters)) {
				auto d = e | kHelQueueWaiters;
				if(__atomic_compare_exchange_n(&_queue->kernelState,
						&e, d, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
					e = d;
			}else{
				HEL_CHECK(helFutexWait((int *)&_queue->kernelState, e));
				e = __atomic_load_n(&_queue->kernelState, __ATOMIC_ACQUIRE);
			}
		}
	}

	void trim() {
		if(!_queue)
			return;

		// for now we just reset the queue.
		_queue->kernelState = 0;
		_queue->userState = 0;
		_progress = 0;
	}

private:
	HelQueue *_queue;
	size_t _progress;
};

thread_local Queue globalQueue;

using FileMap = frigg::Hashmap<
	int,
	HelHandle,
	frigg::DefaultHasher<int>,
	MemoryAllocator
>;

FileMap &getFileMap() {
	static FileMap singleton(frigg::DefaultHasher<int>(), getAllocator());
	return singleton;
}

void __mlibc_initFs() {
	struct FileEntry {
		int fd;
		HelHandle pipe;
	};

	unsigned long openfiles;
	if(!peekauxval(AT_OPENFILES, &openfiles)) {
		for(auto entry = (FileEntry *)openfiles; entry->fd != -1; ++entry)
			getFileMap().insert(entry->fd, entry->pipe);
	}
}

int __mlibc_pushFd(HelHandle handle) {
	// TODO: limit the number of FDs?
	for(int fd = 0; ; fd++) {
		auto it = getFileMap().get(fd);
		if(it)
			continue;
		getFileMap().insert(fd, handle);
		return fd;
	}
}

HelHandle __mlibc_getPassthrough(int fd) {
	auto file_it = getFileMap().get(fd);
	assert(file_it);
	return *file_it;
}

int stat(const char *__restrict path, struct stat *__restrict result) {
	assert(!"Fix this");
	int fd = open(path, O_RDONLY);
	if(fd == -1) {
		__ensure(errno == ENOENT);
		return -1;
	}

	if(fstat(fd, result))
		__ensure("Could not fstat() internal file");

	if(close(fd))
		__ensure("Could not close() internal file");
	return 0;
}

int fstat(int fd, struct stat *result) {
	assert(!"Fix this");
/*	managarm::posix::CntRequest<MemoryAllocator> request(getAllocator());
	request.set_request_type(managarm::posix::CntReqType::FSTAT);
	request.set_fd(fd);

	int64_t request_num = allocPosixRequest();
	frigg::String<MemoryAllocator> serialized(getAllocator());
	request.SerializeToString(&serialized);
	HelError error;
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_num, 0, error);
	HEL_CHECK(error);

	int8_t buffer[128];
	size_t length;
	HelError response_error;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_num, 0, response_error, length);
	HEL_CHECK(response_error);

	managarm::posix::SvrResponse<MemoryAllocator> response(getAllocator());
	response.ParseFromArray(buffer, length);
	if(response.error() == managarm::posix::Errors::SUCCESS) {
		memset(result, 0, sizeof(struct stat));
		result->st_dev = 1;
		result->st_ino = response.inode_num();
		result->st_mode = response.mode() | S_IFREG;
		result->st_nlink = response.num_links();
		result->st_uid = response.uid();
		result->st_gid = response.gid();
		result->st_rdev = 0;
		result->st_size = response.file_size();
		result->st_atim.tv_sec = response.atime_secs();
		result->st_atim.tv_nsec = response.atime_nanos();
		result->st_mtim.tv_sec = response.mtime_secs();
		result->st_mtim.tv_nsec = response.mtime_nanos();
		result->st_ctim.tv_sec = response.ctime_secs();
		result->st_ctim.tv_nsec = response.ctime_nanos();
		result->st_blksize = 4096;
		result->st_blocks = response.file_size() / 512 + 1;
		return 0;
	}else{
		__ensure(!"Unexpected error");
		__builtin_unreachable();
	}*/
}

int open(const char *path, int flags, ...) {
//	frigg::infoLogger.log() << "mlibc: open(\""
//			<< path << "\") called!" << frigg::EndLog();
	HelAction actions[4];
	HelSimpleResult *offer;
	HelSimpleResult *send_req;
	HelInlineResult *recv_resp;
	HelHandleResult *pull_lane;

	globalQueue.trim();

	managarm::posix::CntRequest<MemoryAllocator> req(getAllocator());
	req.set_request_type(managarm::posix::CntReqType::OPEN);
	req.set_path(frigg::String<MemoryAllocator>(getAllocator(), path));

	frigg::String<MemoryAllocator> ser(getAllocator());
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvInline;
	actions[2].flags = kHelItemChain;
	actions[3].type = kHelActionPullDescriptor;
	actions[3].flags = 0;
	HEL_CHECK(helSubmitAsync(posixPipe, actions, 4,
			globalQueue.getQueue(), 0));

	offer = (HelSimpleResult *)globalQueue.dequeueSingle();
	send_req = (HelSimpleResult *)globalQueue.dequeueSingle();
	recv_resp = (HelInlineResult *)globalQueue.dequeueSingle();
	pull_lane = (HelHandleResult *)globalQueue.dequeueSingle();

	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(recv_resp->error);
	HEL_CHECK(pull_lane->error);
	
	managarm::posix::SvrResponse<MemoryAllocator> resp(getAllocator());
	resp.ParseFromArray(recv_resp->data, recv_resp->length);
	if(resp.error() == managarm::posix::Errors::FILE_NOT_FOUND) {
		errno = ENOENT;
		return -1;
	}else{
		assert(resp.error() == managarm::posix::Errors::SUCCESS);
		return __mlibc_pushFd(pull_lane->handle);
	}
}

ssize_t read(int fd, void *data, size_t max_size){
	//frigg::infoLogger() << "read() " << max_size << frigg::EndLog();
	HelAction actions[4];
	HelSimpleResult *offer;
	HelSimpleResult *send_req;
	HelInlineResult *recv_resp;
	HelLengthResult *recv_data;

	globalQueue.trim();

	auto file_it = getFileMap().get(fd);
	assert(file_it);

	managarm::fs::CntRequest<MemoryAllocator> req(getAllocator());
	req.set_req_type(managarm::fs::CntReqType::READ);
	req.set_fd(fd);
	req.set_size(max_size);

	frigg::String<MemoryAllocator> ser(getAllocator());
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvInline;
	actions[2].flags = kHelItemChain;
	actions[3].type = kHelActionRecvToBuffer;
	actions[3].flags = 0;
	actions[3].buffer = data;
	actions[3].length = max_size;
	HEL_CHECK(helSubmitAsync(*file_it, actions, 4,
			globalQueue.getQueue(), 0));

	offer = (HelSimpleResult *)globalQueue.dequeueSingle();
	send_req = (HelSimpleResult *)globalQueue.dequeueSingle();
	recv_resp = (HelInlineResult *)globalQueue.dequeueSingle();
	recv_data = (HelLengthResult *)globalQueue.dequeueSingle();

	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(recv_resp->error);
	HEL_CHECK(recv_data->error);

	managarm::fs::SvrResponse<MemoryAllocator> resp(getAllocator());
	resp.ParseFromArray(recv_resp->data, recv_resp->length);
/*	if(resp.error() == managarm::fs::Errors::NO_SUCH_FD) {
		errno = EBADF;
		return -1;
	}else*/ if(resp.error() == managarm::fs::Errors::END_OF_FILE) {
		return 0;
	}
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	return recv_data->length;
}

ssize_t write(int fd, const void *data, size_t size) {
	HelAction actions[4];
	HelSimpleResult *offer;
	HelSimpleResult *send_req;
	HelSimpleResult *send_data;
	HelInlineResult *recv_resp;

	globalQueue.trim();

	auto file_it = getFileMap().get(fd);
	assert(file_it);

//	frigg::infoLogger.log() << "write()" << frigg::EndLog();
	managarm::fs::CntRequest<MemoryAllocator> req(getAllocator());
	req.set_req_type(managarm::fs::CntReqType::WRITE);
	req.set_fd(fd);

	frigg::String<MemoryAllocator> ser(getAllocator());
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionSendFromBuffer;
	actions[2].flags = kHelItemChain;
	actions[2].buffer = const_cast<void *>(data);
	actions[2].length = size;
	actions[3].type = kHelActionRecvInline;
	actions[3].flags = 0;
	HEL_CHECK(helSubmitAsync(*file_it, actions, 4,
			globalQueue.getQueue(), 0));

	offer = (HelSimpleResult *)globalQueue.dequeueSingle();
	send_req = (HelSimpleResult *)globalQueue.dequeueSingle();
	send_data = (HelSimpleResult *)globalQueue.dequeueSingle();
	recv_resp = (HelInlineResult *)globalQueue.dequeueSingle();

	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(send_data->error);
	HEL_CHECK(recv_resp->error);

	managarm::fs::SvrResponse<MemoryAllocator> resp(getAllocator());
	resp.ParseFromArray(recv_resp->data, recv_resp->length);
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	// TODO: implement NO_SUCH_FD
/*	if(resp.error() == managarm::fs::Errors::NO_SUCH_FD) {
		errno = EBADF;
		return -1;
	}else*/ if(resp.error() == managarm::fs::Errors::SUCCESS) {
		//FIXME: handle partial writes
		return size;
	}else{
		__ensure(!"Unexpected error");
		__builtin_unreachable();
	}
}

off_t lseek(int fd, off_t offset, int whence) {
	assert(!"Fix this");
	/*HelAction actions[3];
	HelEvent results[3];

	auto file_it = getFileMap().get(fd);
	assert(file_it);
	
	managarm::fs::CntRequest<MemoryAllocator> req(getAllocator());
	req.set_fd(fd);
	req.set_rel_offset(offset);

	if(whence == SEEK_SET) {
		req.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
	}else if(whence == SEEK_CUR) {
		req.set_req_type(managarm::fs::CntReqType::SEEK_REL);
	}else if(whence == SEEK_END) {
		req.set_req_type(managarm::fs::CntReqType::SEEK_EOF);
	}else{
		frigg::panicLogger() << "Illegal whence argument" << frigg::endLog;
	}
	
	frigg::String<MemoryAllocator> ser(getAllocator());
	req.SerializeToString(&ser);
	uint8_t buffer[128];
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvToBuffer;
	actions[2].flags = 0;
	actions[2].buffer = buffer;
	actions[2].length = 128;
	//HEL_CHECK(helSubmitAsync(file_it->getHandle(), actions, 3, eventHub.getHandle(), 0));

	results[0] = eventHub.waitForEvent(0);
	results[1] = eventHub.waitForEvent(0);
	results[2] = eventHub.waitForEvent(0);

	HEL_CHECK(results[0].error);
	HEL_CHECK(results[1].error);
	HEL_CHECK(results[2].error);

	managarm::fs::SvrResponse<MemoryAllocator> resp(getAllocator());
	resp.ParseFromArray(buffer, results[2].length);
	*//*if(resp.error() == managarm::fs::Errors::NO_SUCH_FD) {
		errno = EBADF;		
		return -1;
	}else*//* if(resp.error() == managarm::fs::Errors::SUCCESS) {
		return resp.offset();
	}else{
		__ensure(!"Unexpected error");
		__builtin_unreachable();
	}*/
}

HelHandle __raw_map(int fd) {
	HelAction actions[4];
	HelSimpleResult *offer;
	HelSimpleResult *send_req;
	HelInlineResult *recv_resp;
	HelHandleResult *pull_memory;

	globalQueue.trim();

	auto file_it = getFileMap().get(fd);
	assert(file_it);
	
	managarm::fs::CntRequest<MemoryAllocator> req(getAllocator());
	req.set_req_type(managarm::fs::CntReqType::MMAP);
	req.set_fd(fd);
	
	frigg::String<MemoryAllocator> ser(getAllocator());
	req.SerializeToString(&ser);
	actions[0].type = kHelActionOffer;
	actions[0].flags = kHelItemAncillary;
	actions[1].type = kHelActionSendFromBuffer;
	actions[1].flags = kHelItemChain;
	actions[1].buffer = ser.data();
	actions[1].length = ser.size();
	actions[2].type = kHelActionRecvInline;
	actions[2].flags = kHelItemChain;
	actions[3].type = kHelActionPullDescriptor;
	actions[3].flags = 0;
	HEL_CHECK(helSubmitAsync(*file_it, actions, 4,
			globalQueue.getQueue(), 0));

	offer = (HelSimpleResult *)globalQueue.dequeueSingle();
	send_req = (HelSimpleResult *)globalQueue.dequeueSingle();
	recv_resp = (HelInlineResult *)globalQueue.dequeueSingle();
	pull_memory = (HelHandleResult *)globalQueue.dequeueSingle();

	HEL_CHECK(offer->error);
	HEL_CHECK(send_req->error);
	HEL_CHECK(recv_resp->error);
	HEL_CHECK(pull_memory->error);
	
	managarm::fs::SvrResponse<MemoryAllocator> resp(getAllocator());
	resp.ParseFromArray(recv_resp->data, recv_resp->length);
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	return pull_memory->handle;
}

int close(int fd) {
	assert(!"Fix this");
/*	managarm::posix::CntRequest<MemoryAllocator> request(getAllocator());
	request.set_request_type(managarm::posix::CntReqType::CLOSE);
	request.set_fd(fd);

	int64_t request_num = allocPosixRequest();
	frigg::String<MemoryAllocator> serialized(getAllocator());
	request.SerializeToString(&serialized);
	HelError error;
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_num, 0, error);
	HEL_CHECK(error);

	uint8_t buffer[128];
	size_t length;
	HelError response_error;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_num, 0, response_error, length);
	HEL_CHECK(response_error);

	managarm::posix::SvrResponse<MemoryAllocator> response(getAllocator());
	response.ParseFromArray(buffer, length);
	if(response.error() == managarm::posix::Errors::NO_SUCH_FD) {
		errno = EBADF;		
		return -1;
	}else if(response.error() == managarm::posix::Errors::SUCCESS) {
		return 0;
	}else{
		__ensure(!"Unexpected error");
		__builtin_unreachable();
	}*/
}

int dup2(int src_fd, int dest_fd) {
	assert(!"Fix this");
/*	managarm::posix::CntRequest<MemoryAllocator> request(getAllocator());
	request.set_request_type(managarm::posix::CntReqType::DUP2);
	request.set_fd(src_fd);
	request.set_newfd(dest_fd);

	int64_t request_num = allocPosixRequest();
	frigg::String<MemoryAllocator> serialized(getAllocator());
	request.SerializeToString(&serialized);
	HelError error;
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_num, 0, error);
	HEL_CHECK(error);

	int8_t buffer[128];
	size_t length;
	HelError response_error;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_num, 0, response_error, length);
	HEL_CHECK(response_error);

	managarm::posix::SvrResponse<MemoryAllocator> response(getAllocator());
	response.ParseFromArray(buffer, length);
	if(response.error() == managarm::posix::Errors::SUCCESS) {
		return dest_fd;
	}else if(response.error() ==  managarm::posix::Errors::NO_SUCH_FD) {
		errno = EBADF;
		return -1;
	}else {
		__ensure(!"Unexpected error");
		__builtin_unreachable();
	}*/
}

int fcntl(int, int, ...) {
	frigg::infoLogger() << "mlibc: Broken fcntl() called!" << frigg::endLog;
	return 0;
}

int isatty(int fd) {
	assert(!"Fix this");
/*	managarm::posix::CntRequest<MemoryAllocator> request(getAllocator());
	request.set_request_type(managarm::posix::CntReqType::TTY_NAME);
	request.set_fd(fd);

	int64_t request_num = allocPosixRequest();
	frigg::String<MemoryAllocator> serialized(getAllocator());
	request.SerializeToString(&serialized);
	HelError error;
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_num, 0, error);
	HEL_CHECK(error);

	int8_t buffer[128];
	size_t length;
	HelError response_error;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_num, 0, response_error, length);
	HEL_CHECK(response_error);

	managarm::posix::SvrResponse<MemoryAllocator> response(getAllocator());
	response.ParseFromArray(buffer, length);
	if(response.error() == managarm::posix::Errors::SUCCESS) {
		return 1;
	}else if(response.error() ==  managarm::posix::Errors::BAD_FD) {
		errno = ENOTTY;
		return 0;
	}else {
		__ensure(!"Unexpected error");
		__builtin_unreachable();
	}*/
}

char *ttyname(int fd) {
	assert(!"Fix this");
	// TODO: this is not thread-safe.
/*	frigg::String<MemoryAllocator> cache(getAllocator());
	
	managarm::posix::CntRequest<MemoryAllocator> request(getAllocator());
	request.set_request_type(managarm::posix::CntReqType::TTY_NAME);
	request.set_fd(fd);

	int64_t request_num = allocPosixRequest();
	frigg::String<MemoryAllocator> serialized(getAllocator());
	request.SerializeToString(&serialized);
	HelError error;
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_num, 0, error);
	HEL_CHECK(error);

	int8_t buffer[128];
	size_t length;
	HelError response_error;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_num, 0, response_error, length);
	HEL_CHECK(response_error);

	managarm::posix::SvrResponse<MemoryAllocator> response(getAllocator());
	response.ParseFromArray(buffer, length);
	if(response.error() == managarm::posix::Errors::SUCCESS) {
		cache = response.path();
		return cache.data();
	}else if(response.error() ==  managarm::posix::Errors::BAD_FD) {
		errno = ENOTTY;
		return nullptr;
	}else {
		__ensure(!"Unexpected error");
		__builtin_unreachable();
	}*/
}

int tcgetattr(int fd, struct termios *attr) {
	frigg::infoLogger() << "mlibc: Broken tcgetattr() called!" << frigg::endLog;
	attr->c_iflag = 0;
	attr->c_oflag = 0;
	attr->c_cflag = 0;
	attr->c_lflag = ECHO;
	for(size_t i = 0; i < NCCS; i++)
		attr->c_cc[i] = 0;
	attr->c_cc[VMIN] = 1;
	attr->c_cc[VTIME] = 0;
	return 0;
}

int tcsetattr(int, int, const struct termios *attr) {
	frigg::infoLogger() << "mlibc: Broken tcsetattr("
			<< (void *)attr->c_iflag << ", " << (void *)attr->c_oflag
			<< ", " << (void *)attr->c_cflag << ", " << (void *)attr->c_lflag
			<< ") called!" << frigg::endLog;
	return 0;
}

