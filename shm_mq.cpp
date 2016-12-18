
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <list>
#include <signal.h>

#include <pthread.h>

#include <vector>


/**
 * TODO::1. 当 SlabAllocator 不够内存时，应该 new 一个 chunk 来存数据,
 *       然后放入 另一个 backlog 队列，等待 SlabAllocator有内存申请时，再压入
 *       内存队列
 *
 *       2. 当 IoThread 发现 队列的很空时，写入 pipe 通知 部分 worker 做 sleep
 *          当 IoThread 发现 队列任务将要滿时，写入 pipe通知，取消 sleep
 *          当 IoThread 发现 队列 已经满了, 并写入 pipe通知并入队拒绝(还需要尽力写出数据),
 *          Worker应该走 快速消费策略 来 加快处理队列请求
 *
 *      3. Worker应该写pipe通知 IoThread 有回应数据
 */

#define kChunkNULL -1
 #define CACHE_LINE_SIZE 128

typedef int32_t SpinLocker;

inline void CpuPause() { __asm__("pause"); }

typedef int32_t SpinLocker;
// static inline void SpinLockInit(SpinLocker* l)
// {
//   *l = 0;
// }

// static inline void SpinLock(SpinLocker* l)
// {
//   int i;
//   while (__sync_lock_test_and_set(l, 1)){
//     for (i = 0; i < 512; i++){
//       __asm__ volatile("pause\n" ::: "memory");
//     }

//     if (*l == 1) sched_yield();
//   }
// };

// static inline void SpinUnLock(SpinLocker* l)
// {
//   __sync_lock_release(l);
// }

inline void SpinLockInit(SpinLocker *lock) { *lock = 0; }

inline void SpinLock(SpinLocker *lock) {
  for (;;) {
    int32_t val = *lock;
    if ((val & 0x80000000) == 0 &&
        __sync_bool_compare_and_swap(lock, val, val | 0x80000000)) {
      return;
    }

    for (int n = 0; n < 2048; n <<= 1) {
      for (int i = 0; i < n; ++i) {
       __asm__ volatile("pause\n" ::: "memory");
      }

      val = *lock;
      if ((val & 0x80000000) == 0 &&
          __sync_bool_compare_and_swap(lock, val, val | 0x80000000)) {
        return;
      }
    }

    sched_yield();
  }
}

inline void SpinUnLock(SpinLocker *lock) {
  for (;;) {
    int32_t old = *lock;
    int32_t wait = old & 0x7fffffff;
    int32_t val = wait ? wait - 1 : 0;

    if (__sync_bool_compare_and_swap(lock, old, val)) {
      break;
    }
  }
}

class ShareMemory {
public:
  explicit ShareMemory(const size_t size) : m_memPtr(MAP_FAILED), m_size(size) {
    m_memPtr =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  }

  explicit ShareMemory(const std::string &filepath, size_t size,
                       mode_t mode = 0666)
      : m_memPtr(MAP_FAILED), m_size(size) {
    int fd = open(filepath.c_str(), O_CREAT | O_RDWR, mode);
    if (fd != -1) {
      if (ftruncate(fd, size) == 0) {
        m_memPtr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      }
      close(fd);
    }
  }

  explicit ShareMemory(const key_t key, size_t size)
      : m_memPtr(MAP_FAILED), m_size(size) {
    void *addr = GetShmMem(key, size, 0666);
    if (addr == NULL) {
      addr = GetShmMem(key, size, 0666 | IPC_CREAT);
      if (addr != NULL) {
        m_memPtr = addr;
      }
    }
  }

  explicit ShareMemory(void *memory, size_t size)
      : m_memPtr(memory), m_size(size) {}

  bool IsValid() const { return m_memPtr != MAP_FAILED; }

  template <class Type> Type *Ptr() const {
    return reinterpret_cast<Type *>(m_memPtr);
  }

  template <class Type> Type *Ptr(size_t offset) const {
    return reinterpret_cast<Type *>(Ptr<char>() + offset);
  }

  size_t Size() const { return m_size; }

private:
  static void *GetShmMem(key_t key, size_t size, int flag) {
    int shmId = shmget(key, size, flag);
    if (shmId > 0) {
      void *mem = shmat(shmId, NULL, 0);
      if (mem != (void *)-1) {
        return mem;
      }
    }
    return NULL;
  }

private:
  void *m_memPtr;
  const size_t m_size;
};

class MemoryAllocator {
public:
  virtual ShareMemory *Allocate(size_t memSize) = 0;
};

class MMapMemoryAllocator : public MemoryAllocator {
public:
  virtual ShareMemory *Allocate(size_t memSize) {
    return new ShareMemory(memSize);
  }
};

class ShmMemMemoryAllocator : public MemoryAllocator {
public:
  ShmMemMemoryAllocator(const key_t key) : m_key(key) {}

  virtual ShareMemory *Allocate(size_t memSize) {
    return new ShareMemory(m_key, memSize);
  }

private:
  const key_t m_key;
};

class FileMMapMemoryAllocator : public MemoryAllocator {
public:
  FileMMapMemoryAllocator(const std::string filepath) : m_filepath(filepath) {}

  virtual ShareMemory *Allocate(size_t memSize) {
    return new ShareMemory(m_filepath, memSize);
  }

private:
  const std::string m_filepath;
};

class GlibMemoryAllocator : public MemoryAllocator {

  virtual ShareMemory *Allocate(size_t memSize) {
    return new ShareMemory((void *)new char[memSize], memSize);
  }
};

#pragma pack(1)

struct ChunkHeader {
  int32_t m_nextChunk;
  int32_t m_listSize;
  short m_blockId;
};

struct SlabHeader {
  int32_t m_sizeClass;
  int32_t m_eleNum;
  int32_t m_blockNum;
  int64_t m_nextBlockIdx;
};


struct SuperBlock {
  int32_t m_headLock;

  char m_cacheLinePadding1[CACHE_LINE_SIZE];
  int32_t m_tailLock;
  
  char m_cacheLinePadding2[CACHE_LINE_SIZE];
  int64_t m_head;
  
  char m_cacheLinePadding3[CACHE_LINE_SIZE];
  int64_t m_tail;
  
  char m_cacheLinePadding4[CACHE_LINE_SIZE];
  int32_t m_chunkPtrs[];
};
typedef SuperBlock* SuperBlockPtr;


#pragma pack()

class SlabAllocator;
class Chunk {
  friend class SlabAllocator;

public:
  Chunk(SlabAllocator *allocator)
      : m_allocator(allocator), m_chunkOffset(kChunkNULL) {}

  Chunk(int32_t chunkOffset, SlabAllocator *allocator);
  size_t Read(int fd, size_t size);

  size_t Write(int fd, size_t size);

  bool Dump(void *dst, size_t size);

  bool Fill(const void *src, size_t size);

  int32_t Offset() const;

  void Offset(int32_t offset);

  int32_t Used() const;

  bool IsValid() const;

  void DumpInfo();

private:
  int32_t m_chunkOffset;
  int32_t m_used;
  SlabAllocator *m_allocator;
};

class SlabAllocator {
  friend class Chunk;

public:
  SlabAllocator(size_t sizeClass, size_t eleNum, size_t blockNum,
                MemoryAllocator *allocator) {
    size_t chunkRealSize = (sizeof(ChunkHeader) + sizeClass);
    int chunkPtrsPadding = sizeof(int32_t) * eleNum;

    size_t superBlockSize = sizeof(SuperBlock) +    //头
                            chunkPtrsPadding +      // m_chunkPtrs数组
                            chunkRealSize * eleNum; // chunk数据

    size_t memSize = sizeof(SlabHeader) + superBlockSize * blockNum;

    m_sharedMemory = allocator->Allocate(memSize);
    if (m_sharedMemory == NULL) {
      return;
    }

    if (!m_sharedMemory->IsValid()) {
      return;
    }

    m_superBlockSize = superBlockSize;

    m_slab = m_sharedMemory->Ptr<SlabHeader>();
    m_slab->m_sizeClass = sizeClass;
    m_slab->m_eleNum = eleNum;
    m_slab->m_blockNum = blockNum;
    m_slab->m_nextBlockIdx = 0;

    m_blocks = new SuperBlockPtr[blockNum];

    for (int i = 0; i < blockNum; ++i) {
      size_t offset = sizeof(SlabHeader) + superBlockSize * i;
      SuperBlock *block = m_sharedMemory->Ptr<SuperBlock>(offset);

      SpinLockInit(&block->m_headLock);
      SpinLockInit(&block->m_tailLock);
      block->m_head = 0;
      block->m_tail = eleNum;

      for (int j = 0; j < eleNum; ++j) {
        block->m_chunkPtrs[j] = offset + sizeof(SuperBlock) + chunkPtrsPadding + chunkRealSize * j;
        GetChunkHeader(block->m_chunkPtrs[j])->m_blockId = i; //blockId
      }

      m_blocks[i] = block;
    }
  }

public:
  void Dump()
  {
    size_t chunkRealSize = (sizeof(ChunkHeader) + m_slab->m_sizeClass);
    int chunkPtrsPadding = sizeof(int32_t) * m_slab->m_eleNum;

    size_t superBlockSize = sizeof(SuperBlock) +    //头
                            chunkPtrsPadding +      // m_chunkPtrs数组
                            chunkRealSize * m_slab->m_eleNum; // chunk数据

    size_t memSize = sizeof(SlabHeader) + superBlockSize * m_slab->m_blockNum;

    printf("##############\n");                  
    printf("Slab Info:\n");
    printf("totalSize: %d, blockSize:%d, sizeClass : %d, eleNum: %d, blockNum: %d, robinCnt: %lld\n", memSize, superBlockSize,
      m_slab->m_sizeClass, m_slab->m_eleNum, m_slab->m_blockNum, m_slab->m_nextBlockIdx);
    
    for (int i = 0; i < m_slab->m_blockNum; ++i)
    {
      size_t offset = sizeof(SlabHeader) + superBlockSize * i;
      SuperBlock *block = m_sharedMemory->Ptr<SuperBlock>(offset);
      printf("--------------\n");
      printf("Block %d:\n", i);
      printf("head: %d, tail: %d\n", block->m_head & (m_slab->m_eleNum - 1), block->m_tail & (m_slab->m_eleNum - 1));
      printf("chunk offset: \n");
      for (int j = 0; j < m_slab->m_eleNum; ++j)
      {
        printf("%d", block->m_chunkPtrs[j]);
        if(j != m_slab->m_eleNum -1)
        {
          printf(" -> ");
        }
      }
      printf("\n");
    }
    printf("##############\n");
  
  }

private:
  int32_t Allocate() {
    int32_t blockIdx =
        __sync_fetch_and_add(&m_slab->m_nextBlockIdx, 1) % m_slab->m_blockNum;

    // robin 分配一个 chunk
    int32_t chunkOffset = BlockAllocate(blockIdx);
    if (chunkOffset != kChunkNULL) {
      return chunkOffset;
    }

    // 当前 block 空闲不足，则查找别的block
    for (int i = 0; i < m_slab->m_blockNum; ++i) {
      if (i != blockIdx) {
        chunkOffset = BlockAllocate(i);
        if (chunkOffset != kChunkNULL) {
          return chunkOffset;
        }
      }
    }

    return kChunkNULL;
  }

public:
  Chunk Allocate(size_t size) {
    int32_t chunkOffset = _Allocate(size);
    return Chunk(chunkOffset, this);
  }

  void Release(Chunk chunk) {
    int32_t chunkOffset = chunk.Offset();
    Release(chunkOffset);
    chunk.Offset(kChunkNULL);
  }

private:
  int32_t _Allocate(size_t size) {
    if (size <= m_slab->m_sizeClass) {
      return Allocate();
    }
    int32_t sizeClass = m_slab->m_sizeClass;
    int needAllocNum = (size + sizeClass - 1) / sizeClass;

    int32_t list = Allocate();
    if (list == kChunkNULL) {
      return kChunkNULL;
    }

    GetChunkHeader(list)->m_listSize = needAllocNum * sizeClass;

    int32_t current = list;
    int32_t next;

    for (int i = 0; i < needAllocNum - 1; ++i) {
      next = Allocate();
      if (next == kChunkNULL) {
        break;
      }

      GetChunkHeader(current)->m_nextChunk = next;
      current = next;
    }

    if (next == kChunkNULL) {
      Release(list);
      return kChunkNULL;
    }

    return list;
  }

  void Release(int32_t chunkOffset) {
    int32_t current = chunkOffset;
    int32_t chunkPtrsPadding = sizeof(int32_t) * m_slab->m_eleNum;

    while (current > 0 && current <= m_sharedMemory->Size()) {
      ChunkHeader *header = GetChunkHeader(current);
      int32_t nextChunk = header->m_nextChunk;

      header->m_nextChunk = kChunkNULL;
      header->m_listSize = 0;
      memset(GetChunkData(header), 0, m_slab->m_sizeClass);

      int32_t blockIdx = header->m_blockId;
      BlockRelease(blockIdx, current);

      current = nextChunk;
    }
  }

  void BlockRelease(int32_t blockIdx, int32_t chunkOffset) {
    SuperBlock *block = m_blocks[blockIdx];

    SpinLock(&block->m_tailLock);

    int64_t tail = block->m_tail;
    if (block->m_head + m_slab->m_eleNum == tail) {
      SpinUnLock(&block->m_tailLock);
      return;
    }
  
    ++block->m_tail;
    SpinUnLock(&block->m_tailLock);

    block->m_chunkPtrs[tail & (m_slab->m_eleNum - 1)] = chunkOffset;
  }

  // 这里可以减少 spinlock的粒度: 采用先占坑, 再埋坑的策略
  int32_t BlockAllocate(int32_t blockIdx) {
    SuperBlock *block = m_blocks[blockIdx];

    SpinLock(&block->m_headLock);

    int64_t head = block->m_head;
    int64_t tail = block->m_tail;

    if (head == tail) {
      SpinUnLock(&block->m_headLock);
      return kChunkNULL;
    }

    int64_t mask = head & (m_slab->m_eleNum - 1);
    int32_t chunkOffset = block->m_chunkPtrs[mask];

    if (chunkOffset == kChunkNULL) {
      SpinUnLock(&block->m_headLock);
      return kChunkNULL;
    }

    block->m_chunkPtrs[mask] = kChunkNULL;
    block->m_head += 1;

    SpinUnLock(&block->m_headLock);

    ChunkHeader *header = GetChunkHeader(chunkOffset);
    header->m_nextChunk = kChunkNULL;
    header->m_listSize = m_slab->m_sizeClass;

    return chunkOffset;
  }

  inline ChunkHeader *GetChunkHeader(int32_t chunkOffset) {
    return m_sharedMemory->Ptr<ChunkHeader>(chunkOffset);
  }

  inline void *GetChunkData(int32_t chunkOffset) {
    return (char *)GetChunkHeader(chunkOffset) + sizeof(ChunkHeader);
  }

  inline void *GetChunkData(ChunkHeader *chunkHeader) {
    return (char *)chunkHeader + sizeof(chunkHeader);
  }

private:
  ShareMemory *m_sharedMemory;
  SlabHeader *m_slab;
  SuperBlock **m_blocks;
  size_t m_superBlockSize;
};

Chunk::Chunk(int32_t chunkOffset, SlabAllocator *allocator)
    : m_chunkOffset(chunkOffset), m_allocator(allocator) {}
size_t Chunk::Read(int fd, size_t size) { return -1; }

size_t Chunk::Write(int fd, size_t size) { return -1; }

bool Chunk::Dump(void *dst, size_t size) {

  if (!IsValid()) {
    return false;
  }

  int listSize = m_allocator->GetChunkHeader(m_chunkOffset)->m_listSize;
  int sizeClass = m_allocator->m_slab->m_sizeClass;

  int left = size < listSize ? size : listSize;
  int offset = 0;

  int32_t current = m_chunkOffset;

  while (left > 0) {
    int perSize = sizeClass < left ? sizeClass : left;

    memcpy((char *)dst + offset, m_allocator->GetChunkData(current), perSize);

    offset += perSize;
    left -= perSize;

    current = m_allocator->GetChunkHeader(current)->m_nextChunk;
  }
  return size >= listSize;
}

void Chunk::DumpInfo()
{  int32_t listSize = m_allocator->GetChunkHeader(m_chunkOffset)->m_listSize;
  printf("\n###Chunk total: %d####\n", listSize);
  int32_t current = m_chunkOffset;
  while(current > 0)
  {
    printf("%d -> ", current);
    current = m_allocator->GetChunkHeader(current)->m_nextChunk;
  }
  printf("\n###############\n");
}

bool Chunk::Fill(const void *src, size_t size) {

  if (!IsValid()) {
    return false;
  }

  int32_t listSize = m_allocator->GetChunkHeader(m_chunkOffset)->m_listSize;
  int sizeClass = m_allocator->m_slab->m_sizeClass;

  int left = listSize < size ? listSize : size;
  int offset = 0;

  int32_t current = m_chunkOffset;
  while (left > 0) {
    int perSize = sizeClass < left ? sizeClass : left;

    memcpy(m_allocator->GetChunkData(current), (char *)src + offset, perSize);

    offset += perSize;
    left -= perSize;
    current = m_allocator->GetChunkHeader(current)->m_nextChunk;
  }
  m_used = size;
  return listSize >= size;
}

int32_t Chunk::Offset() const { return m_chunkOffset; }

void Chunk::Offset(int32_t offset) { m_chunkOffset = offset; }

int32_t Chunk::Used() const { return m_used; }

bool Chunk::IsValid() const {
  return m_chunkOffset != kChunkNULL && m_allocator != NULL;
}

class QueueLock {
public:
  QueueLock(int32_t *lock) {}
  inline void Lock() {}
  inline void UnLock(){};
};

class SpinQueueLock {
public:
  SpinQueueLock(int32_t *lock) : m_lock(lock) {
    SpinLockInit(m_lock);
  }

  inline void Lock() { SpinLock(m_lock); }

  inline void UnLock() { SpinUnLock(m_lock); }

private:
  int32_t *m_lock;
};

#pragma pack(1)

typedef int32_t ChunkMsg;

struct QueueHeader {
  int32_t m_queueSize;

  int64_t m_head;
  char m_cachelinePadding1[CACHE_LINE_SIZE];
  int64_t m_tail;
  char m_cachelinePadding2[CACHE_LINE_SIZE];

  int32_t m_headLock;
  char m_cachelinePadding3[CACHE_LINE_SIZE];
  int32_t m_tailLock;
  char m_cachelinePadding4[CACHE_LINE_SIZE];

  ChunkMsg m_queuePtrs[];
};

#pragma pack()

template <class HeadLockType, class TailLockType> class ChunkQueue {
public:
  ChunkQueue(size_t queueSize, MemoryAllocator *allocator) {
    int memSize = queueSize * sizeof(ChunkMsg) + sizeof(QueueHeader);
    m_sharedMemory = allocator->Allocate(memSize);

    if (m_sharedMemory == NULL) {
      return;
    }

    if (!m_sharedMemory->IsValid()) {
      return;
    }

    m_queue = m_sharedMemory->Ptr<QueueHeader>();

    m_queue->m_queueSize = queueSize;
    m_queue->m_head = 0;
    m_queue->m_tail = 0;

    m_headLock = new HeadLockType(&m_queue->m_headLock);
    m_tailLock = new TailLockType(&m_queue->m_tailLock);

    for (int i = 0; i < m_queue->m_queueSize; ++i) {
      m_queue->m_queuePtrs[i] = kChunkNULL;
    }
  }

  void Dump()
  {
    printf("-----------------\n");
    printf("queue size: %d, head=%d, tail=%d\n", m_queue->m_queueSize, m_queue->m_head, m_queue->m_tail);
    printf("items\n");
    for (int i = 0; i < m_queue->m_queueSize; ++i)
    {
      printf("%d -> ", m_queue->m_queuePtrs[i]);
    }
    printf("\n---------------\n");
  }

  bool PushChunk(Chunk chunk) {
    m_tailLock->Lock();

    int64_t tail = m_queue->m_tail;

    if (m_queue->m_head + m_queue->m_queueSize == tail) {
      m_tailLock->UnLock();
      return false;
    }

    m_queue->m_tail += 1;
    m_tailLock->UnLock();

    m_queue->m_queuePtrs[tail & (m_queue->m_queueSize - 1)] = chunk.Offset();

    return true;
  }

  bool PopChunk(Chunk &chunk) {
    m_headLock->Lock();

    if (m_queue->m_head == m_queue->m_tail) {
      m_headLock->UnLock();
      return false;
    }

    int32_t masked = m_queue->m_head & (m_queue->m_queueSize - 1);
    int32_t chunkOffset = m_queue->m_queuePtrs[masked];

    if (chunkOffset == kChunkNULL) {
      m_headLock->UnLock();
      return false;
    }

    m_queue->m_head = m_queue->m_head + 1;
    m_headLock->UnLock();

    chunk.Offset(chunkOffset);
    m_queue->m_queuePtrs[masked] = kChunkNULL;

    return true;
  }

private:
  QueueHeader *m_queue;
  ShareMemory *m_sharedMemory;
  HeadLockType *m_headLock;
  TailLockType *m_tailLock;
};

typedef ChunkQueue<QueueLock, SpinQueueLock> SpmcQueue;
typedef ChunkQueue<SpinQueueLock, QueueLock> MpscQueue;
typedef ChunkQueue<SpinQueueLock, SpinQueueLock> MpmcQueue;
typedef ChunkQueue<QueueLock, QueueLock> SpscQueue;

class RequestQueue {
public:
  RequestQueue(size_t queueSize) : m_success(0) {
    m_queue = new SpmcQueue(queueSize, new MMapMemoryAllocator());
  }

  void PushChunk(Chunk chunk) {

    if (m_queue->PushChunk(chunk)) {
      if (++m_success == kPutSuccessTime) {
        PushBacklogQueue();
        m_success = 0;
      }

    } else {

      m_backlogQueue.push_back(chunk);
    }
  }

  bool PopChunk(Chunk &chunk) { return m_queue->PopChunk(chunk); }

private:
  void PushBacklogQueue() {
    std::list<Chunk>::iterator it = m_backlogQueue.begin();
    for (; it != m_backlogQueue.end(); it++) {
      if (!m_queue->PushChunk(*it)) {
        break;
      }
    }
  }

public:
  void Dump(){
    m_queue->Dump();
  }

private:
  std::list<Chunk> m_backlogQueue;
  int64_t m_success;
  SpmcQueue *m_queue;
  static const int32_t kPutSuccessTime = 3;
};

class ResponseQueue {
public:
  ResponseQueue(size_t queueSize) : m_success(0) {
    m_queue = new MpscQueue(queueSize, new MMapMemoryAllocator());
  }

  void PushChunk(Chunk chunk) {

    if (m_queue->PushChunk(chunk)) {
      if (++m_success == kPutSuccessTime) {
        PushBacklogQueue();
        m_success = 0;
      }

    } else {

      m_backlogQueue.push_back(chunk);
    }
  }

  bool PopChunk(Chunk &chunk) { return m_queue->PopChunk(chunk); }

private:
  void PushBacklogQueue() {
    std::list<Chunk>::iterator it = m_backlogQueue.begin();
    for (; it != m_backlogQueue.end(); it++) {
      if (!m_queue->PushChunk(*it)) {
        break;
      }
    }
  }
public:
    void Dump(){
    m_queue->Dump();
  }

private:
  std::list<Chunk> m_backlogQueue;
  int64_t m_success;
  MpscQueue *m_queue;
  static const int32_t kPutSuccessTime = 3;
};


// 分配 200 MB共享内存
SlabAllocator *slabAllocator = new SlabAllocator(
    64,     /*chunk大小*/
    2 << 8, /*每个block的chunk数目*/
    4,       /* block数目 */
    new MMapMemoryAllocator() /* mmap 共享内存 */);

RequestQueue *reqQueue =
    new RequestQueue(2 << 16); /* 共享请求队列， 一写多读 */

// ResponseQueue *repQueue =
//     new ResponseQueue(2 << 16); /* 共享回应队列, 多写一读 */

static bool g_exit = false;
void StopHandler(int signo) { g_exit = true; }

void *IoThread(void *arg) {
  // 1 个 io 线程
  long idx = 0;
  char buf[64] = "\0";

   snprintf(buf, sizeof(buf),
             "Hello world, Lock Free Queue and SlabAllocator\n");

  while (!g_exit) {
    // step1. 申请内存
    Chunk reqChunk = slabAllocator->Allocate(32);
    if (!reqChunk.IsValid()) {
      // slabAllocator->Dump();
      // printf("slab error!\n");
      // exit(0);
      continue;
    }

   
    // step2. 写入数据
    if (!reqChunk.Fill(buf, strlen(buf))) {
      // printf("fill error\n");
      slabAllocator->Release(reqChunk);
      continue;
    }
    // step3. 放入请求队列
    reqQueue->PushChunk(reqChunk);
    idx = idx + 1;

    // sleep(1);

    // step4. 等待回应队列，Worker进程应该有 通知 io线程机制: 例如 pipe
    // Chunk repChunk(slabAllocator);
    // if (repQueue->PopChunk(repChunk)) {
      
    //   if (repChunk.Dump(buf, 64)) {
    //     printf("reponse: %s", buf);
    //   }

    //   // step5. 释放回应数据
    //   slabAllocator->Release(repChunk);
    // }
  }

  printf("io thread finish!, idx count=%ld\n", idx);
  return (void *)idx;
}

void *WorkerProcess(void *arg) {

  // 8 个 worker
  // fork();
  // fork();
  // fork();

  long idx = 0;
  char buf[1024] = "\0";
  clock_t start = clock();
  while (!g_exit) {
    // step1. 获取请求
    Chunk reqChunk(slabAllocator);
    if (reqQueue->PopChunk(reqChunk)) {

      // step2. 处理请求
      if (reqChunk.Dump(buf, 1024) ){
        // printf("request: %s", buf);
      }

      idx = idx + 1;


      // step3. 释放请求内存
      // printf("offset: %d\n", reqChunk.Offset());
      slabAllocator->Release(reqChunk);
      // slabAllocator->Dump();

      // usleep(200);

      // sleep(1);

      // // step4. 写回应数据
      // Chunk repChunk = slabAllocator->Allocate(32);
      // snprintf(buf, sizeof(buf), "Result: OK, Data: { ... }_%ld\n", idx);

      // if (!repChunk.Fill(buf, strlen(buf))) {
      //   slabAllocator->Release(reqChunk);
      //   continue;
      // }

      // // step5. 放入回应队列
      // repQueue->PushChunk(repChunk);
    
    }else {
      // printf("pop req fail\n");
      // slabAllocator->Dump();
      // sleep(1);
    }
  }

  printf("worker finish! idx count=%ld\n", idx);
  clock_t end = clock();

  double duration = (double)(end - start) / CLOCKS_PER_SEC;

  printf( "take %2.1f seconds, %.2f k/s\n", duration,   idx * 1.0 / 1000 / duration);

  return (void *)idx;
}

int main(int argc, char const *argv[]) {

  signal(SIGINT, StopHandler);

  pid_t pid = fork();
  if (pid > 0) {
    IoThread(NULL);
  } else {

    WorkerProcess(NULL);
  }
}
