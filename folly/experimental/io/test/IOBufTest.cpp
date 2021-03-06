/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "folly/experimental/io/IOBuf.h"
#include "folly/experimental/io/TypedIOBuf.h"

// googletest requires std::tr1::tuple, not std::tuple
#include <tr1/tuple>

#include <gflags/gflags.h>
#include <boost/random.hpp>
#include <gtest/gtest.h>

#include "folly/Malloc.h"
#include "folly/Range.h"

using folly::fbstring;
using folly::IOBuf;
using folly::TypedIOBuf;
using folly::StringPiece;
using std::unique_ptr;

void append(std::unique_ptr<IOBuf>& buf, StringPiece str) {
  EXPECT_LE(str.size(), buf->tailroom());
  memcpy(buf->writableData(), str.data(), str.size());
  buf->append(str.size());
}

void prepend(std::unique_ptr<IOBuf>& buf, StringPiece str) {
  EXPECT_LE(str.size(), buf->headroom());
  memcpy(buf->writableData() - str.size(), str.data(), str.size());
  buf->prepend(str.size());
}

TEST(IOBuf, Simple) {
  unique_ptr<IOBuf> buf(IOBuf::create(100));
  uint32_t cap = buf->capacity();
  EXPECT_LE(100, cap);
  EXPECT_EQ(0, buf->headroom());
  EXPECT_EQ(0, buf->length());
  EXPECT_EQ(cap, buf->tailroom());

  append(buf, "world");
  buf->advance(10);
  EXPECT_EQ(10, buf->headroom());
  EXPECT_EQ(5, buf->length());
  EXPECT_EQ(cap - 15, buf->tailroom());

  prepend(buf, "hello ");
  EXPECT_EQ(4, buf->headroom());
  EXPECT_EQ(11, buf->length());
  EXPECT_EQ(cap - 15, buf->tailroom());

  const char* p = reinterpret_cast<const char*>(buf->data());
  EXPECT_EQ("hello world", std::string(p, buf->length()));

  buf->clear();
  EXPECT_EQ(0, buf->headroom());
  EXPECT_EQ(0, buf->length());
  EXPECT_EQ(cap, buf->tailroom());
}


void testAllocSize(uint32_t requestedCapacity) {
  unique_ptr<IOBuf> iobuf(IOBuf::create(requestedCapacity));
  EXPECT_GE(iobuf->capacity(), requestedCapacity);
}

TEST(IOBuf, AllocSizes) {
  // Try with a small allocation size that should fit in the internal buffer
  testAllocSize(28);

  // Try with a large allocation size that will require an external buffer.
  testAllocSize(9000);

  // 220 bytes is currently the cutoff
  // (It would be nice to use the IOBuf::kMaxInternalDataSize constant,
  // but it's private and it doesn't seem worth making it public just for this
  // test code.)
  testAllocSize(220);
  testAllocSize(219);
  testAllocSize(221);
}

void deleteArrayBuffer(void *buf, void* arg) {
  uint32_t* deleteCount = static_cast<uint32_t*>(arg);
  ++(*deleteCount);
  uint8_t* bufPtr = static_cast<uint8_t*>(buf);
  delete[] bufPtr;
}

TEST(IOBuf, TakeOwnership) {
  uint32_t size1 = 99;
  uint8_t *buf1 = static_cast<uint8_t*>(malloc(size1));
  unique_ptr<IOBuf> iobuf1(IOBuf::takeOwnership(buf1, size1));
  EXPECT_EQ(buf1, iobuf1->data());
  EXPECT_EQ(size1, iobuf1->length());
  EXPECT_EQ(buf1, iobuf1->buffer());
  EXPECT_EQ(size1, iobuf1->capacity());

  uint32_t deleteCount = 0;
  uint32_t size2 = 4321;
  uint8_t *buf2 = new uint8_t[size2];
  unique_ptr<IOBuf> iobuf2(IOBuf::takeOwnership(buf2, size2,
                                                deleteArrayBuffer,
                                                &deleteCount));
  EXPECT_EQ(buf2, iobuf2->data());
  EXPECT_EQ(size2, iobuf2->length());
  EXPECT_EQ(buf2, iobuf2->buffer());
  EXPECT_EQ(size2, iobuf2->capacity());
  EXPECT_EQ(0, deleteCount);
  iobuf2.reset();
  EXPECT_EQ(1, deleteCount);

  deleteCount = 0;
  uint32_t size3 = 3456;
  uint8_t *buf3 = new uint8_t[size3];
  uint32_t length3 = 48;
  unique_ptr<IOBuf> iobuf3(IOBuf::takeOwnership(buf3, size3, length3,
                                                deleteArrayBuffer,
                                                &deleteCount));
  EXPECT_EQ(buf3, iobuf3->data());
  EXPECT_EQ(length3, iobuf3->length());
  EXPECT_EQ(buf3, iobuf3->buffer());
  EXPECT_EQ(size3, iobuf3->capacity());
  EXPECT_EQ(0, deleteCount);
  iobuf3.reset();
  EXPECT_EQ(1, deleteCount);


}

TEST(IOBuf, WrapBuffer) {
  const uint32_t size1 = 1234;
  uint8_t buf1[size1];
  unique_ptr<IOBuf> iobuf1(IOBuf::wrapBuffer(buf1, size1));
  EXPECT_EQ(buf1, iobuf1->data());
  EXPECT_EQ(size1, iobuf1->length());
  EXPECT_EQ(buf1, iobuf1->buffer());
  EXPECT_EQ(size1, iobuf1->capacity());

  uint32_t size2 = 0x1234;
  unique_ptr<uint8_t[]> buf2(new uint8_t[size2]);
  unique_ptr<IOBuf> iobuf2(IOBuf::wrapBuffer(buf2.get(), size2));
  EXPECT_EQ(buf2.get(), iobuf2->data());
  EXPECT_EQ(size2, iobuf2->length());
  EXPECT_EQ(buf2.get(), iobuf2->buffer());
  EXPECT_EQ(size2, iobuf2->capacity());
}

void fillBuf(uint8_t* buf, uint32_t length, boost::mt19937& gen) {
  for (uint32_t n = 0; n < length; ++n) {
    buf[n] = static_cast<uint8_t>(gen() & 0xff);
  }
}

void fillBuf(IOBuf* buf, boost::mt19937& gen) {
  buf->unshare();
  fillBuf(buf->writableData(), buf->length(), gen);
}

void checkBuf(const uint8_t* buf, uint32_t length, boost::mt19937& gen) {
  // Rather than using EXPECT_EQ() to check each character,
  // count the number of differences and the first character that differs.
  // This way on error we'll report just that information, rather than tons of
  // failed checks for each byte in the buffer.
  uint32_t numDifferences = 0;
  uint32_t firstDiffIndex = 0;
  uint8_t firstDiffExpected = 0;
  for (uint32_t n = 0; n < length; ++n) {
    uint8_t expected = static_cast<uint8_t>(gen() & 0xff);
    if (buf[n] == expected) {
      continue;
    }

    if (numDifferences == 0) {
      firstDiffIndex = n;
      firstDiffExpected = expected;
    }
    ++numDifferences;
  }

  EXPECT_EQ(0, numDifferences);
  if (numDifferences > 0) {
    // Cast to int so it will be printed numerically
    // rather than as a char if the check fails
    EXPECT_EQ(static_cast<int>(buf[firstDiffIndex]),
              static_cast<int>(firstDiffExpected));
  }
}

void checkBuf(IOBuf* buf, boost::mt19937& gen) {
  checkBuf(buf->data(), buf->length(), gen);
}

void checkChain(IOBuf* buf, boost::mt19937& gen) {
  IOBuf *current = buf;
  do {
    checkBuf(current->data(), current->length(), gen);
    current = current->next();
  } while (current != buf);
}

TEST(IOBuf, Chaining) {
  uint32_t fillSeed = 0x12345678;
  boost::mt19937 gen(fillSeed);

  // An IOBuf with external storage
  uint32_t headroom = 123;
  unique_ptr<IOBuf> iob1(IOBuf::create(2048));
  iob1->advance(headroom);
  iob1->append(1500);
  fillBuf(iob1.get(), gen);

  // An IOBuf with internal storage
  unique_ptr<IOBuf> iob2(IOBuf::create(20));
  iob2->append(20);
  fillBuf(iob2.get(), gen);

  // An IOBuf around a buffer it doesn't own
  uint8_t localbuf[1234];
  fillBuf(localbuf, 1234, gen);
  unique_ptr<IOBuf> iob3(IOBuf::wrapBuffer(localbuf, sizeof(localbuf)));

  // An IOBuf taking ownership of a user-supplied buffer
  uint32_t heapBufSize = 900;
  uint8_t* heapBuf = static_cast<uint8_t*>(malloc(heapBufSize));
  fillBuf(heapBuf, heapBufSize, gen);
  unique_ptr<IOBuf> iob4(IOBuf::takeOwnership(heapBuf, heapBufSize));

  // An IOBuf taking ownership of a user-supplied buffer with
  // a custom free function
  uint32_t arrayBufSize = 321;
  uint8_t* arrayBuf = new uint8_t[arrayBufSize];
  fillBuf(arrayBuf, arrayBufSize, gen);
  uint32_t arrayBufFreeCount = 0;
  unique_ptr<IOBuf> iob5(IOBuf::takeOwnership(arrayBuf, arrayBufSize,
                                              deleteArrayBuffer,
                                              &arrayBufFreeCount));

  EXPECT_FALSE(iob1->isChained());
  EXPECT_FALSE(iob2->isChained());
  EXPECT_FALSE(iob3->isChained());
  EXPECT_FALSE(iob4->isChained());
  EXPECT_FALSE(iob5->isChained());

  EXPECT_FALSE(iob1->isSharedOne());
  EXPECT_FALSE(iob2->isSharedOne());
  EXPECT_TRUE(iob3->isSharedOne()); // since we own the buffer
  EXPECT_FALSE(iob4->isSharedOne());
  EXPECT_FALSE(iob5->isSharedOne());

  // Chain the buffers all together
  // Since we are going to relinquish ownership of iob2-5 to the chain,
  // store raw pointers to them so we can reference them later.
  IOBuf* iob2ptr = iob2.get();
  IOBuf* iob3ptr = iob3.get();
  IOBuf* iob4ptr = iob4.get();
  IOBuf* iob5ptr = iob5.get();

  iob1->prependChain(std::move(iob2));
  iob1->prependChain(std::move(iob4));
  iob2ptr->appendChain(std::move(iob3));
  iob1->prependChain(std::move(iob5));

  EXPECT_EQ(iob2ptr, iob1->next());
  EXPECT_EQ(iob3ptr, iob2ptr->next());
  EXPECT_EQ(iob4ptr, iob3ptr->next());
  EXPECT_EQ(iob5ptr, iob4ptr->next());
  EXPECT_EQ(iob1.get(), iob5ptr->next());

  EXPECT_EQ(iob5ptr, iob1->prev());
  EXPECT_EQ(iob1.get(), iob2ptr->prev());
  EXPECT_EQ(iob2ptr, iob3ptr->prev());
  EXPECT_EQ(iob3ptr, iob4ptr->prev());
  EXPECT_EQ(iob4ptr, iob5ptr->prev());

  EXPECT_TRUE(iob1->isChained());
  EXPECT_TRUE(iob2ptr->isChained());
  EXPECT_TRUE(iob3ptr->isChained());
  EXPECT_TRUE(iob4ptr->isChained());
  EXPECT_TRUE(iob5ptr->isChained());

  uint64_t fullLength = (iob1->length() + iob2ptr->length() +
                         iob3ptr->length() + iob4ptr->length() +
                         iob5ptr->length());
  EXPECT_EQ(5, iob1->countChainElements());
  EXPECT_EQ(fullLength, iob1->computeChainDataLength());

  // Since iob3 is shared, the entire buffer should report itself as shared
  EXPECT_TRUE(iob1->isShared());
  // Unshare just iob3
  iob3ptr->unshareOne();
  EXPECT_FALSE(iob3ptr->isSharedOne());
  // Now everything in the chain should be unshared.
  // Check on all members of the chain just for good measure
  EXPECT_FALSE(iob1->isShared());
  EXPECT_FALSE(iob2ptr->isShared());
  EXPECT_FALSE(iob3ptr->isShared());
  EXPECT_FALSE(iob4ptr->isShared());
  EXPECT_FALSE(iob5ptr->isShared());


  // Clone one of the IOBufs in the chain
  unique_ptr<IOBuf> iob4clone = iob4ptr->cloneOne();
  gen.seed(fillSeed);
  checkBuf(iob1.get(), gen);
  checkBuf(iob2ptr, gen);
  checkBuf(iob3ptr, gen);
  checkBuf(iob4clone.get(), gen);
  checkBuf(iob5ptr, gen);

  EXPECT_TRUE(iob1->isShared());
  EXPECT_TRUE(iob2ptr->isShared());
  EXPECT_TRUE(iob3ptr->isShared());
  EXPECT_TRUE(iob4ptr->isShared());
  EXPECT_TRUE(iob5ptr->isShared());

  EXPECT_FALSE(iob1->isSharedOne());
  EXPECT_FALSE(iob2ptr->isSharedOne());
  EXPECT_FALSE(iob3ptr->isSharedOne());
  EXPECT_TRUE(iob4ptr->isSharedOne());
  EXPECT_FALSE(iob5ptr->isSharedOne());

  // Unshare that clone
  EXPECT_TRUE(iob4clone->isSharedOne());
  iob4clone->unshare();
  EXPECT_FALSE(iob4clone->isSharedOne());
  EXPECT_FALSE(iob4ptr->isSharedOne());
  EXPECT_FALSE(iob1->isShared());
  iob4clone.reset();


  // Create a clone of a different IOBuf
  EXPECT_FALSE(iob1->isShared());
  EXPECT_FALSE(iob3ptr->isSharedOne());

  unique_ptr<IOBuf> iob3clone = iob3ptr->cloneOne();
  gen.seed(fillSeed);
  checkBuf(iob1.get(), gen);
  checkBuf(iob2ptr, gen);
  checkBuf(iob3clone.get(), gen);
  checkBuf(iob4ptr, gen);
  checkBuf(iob5ptr, gen);

  EXPECT_TRUE(iob1->isShared());
  EXPECT_TRUE(iob3ptr->isSharedOne());
  EXPECT_FALSE(iob1->isSharedOne());

  // Delete the clone and make sure the original is unshared
  iob3clone.reset();
  EXPECT_FALSE(iob1->isShared());
  EXPECT_FALSE(iob3ptr->isSharedOne());


  // Clone the entire chain
  unique_ptr<IOBuf> chainClone = iob1->clone();
  // Verify that the data is correct.
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // Check that the buffers report sharing correctly
  EXPECT_TRUE(chainClone->isShared());
  EXPECT_TRUE(iob1->isShared());

  EXPECT_TRUE(iob1->isSharedOne());
  // since iob2 has a small internal buffer, it will never be shared
  EXPECT_FALSE(iob2ptr->isSharedOne());
  EXPECT_TRUE(iob3ptr->isSharedOne());
  EXPECT_TRUE(iob4ptr->isSharedOne());
  EXPECT_TRUE(iob5ptr->isSharedOne());

  // Unshare the cloned chain
  chainClone->unshare();
  EXPECT_FALSE(chainClone->isShared());
  EXPECT_FALSE(iob1->isShared());

  // Make sure the unshared result still has the same data
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // Destroy this chain
  chainClone.reset();


  // Clone a new chain
  EXPECT_FALSE(iob1->isShared());
  chainClone = iob1->clone();
  EXPECT_TRUE(iob1->isShared());
  EXPECT_TRUE(chainClone->isShared());

  // Delete the original chain
  iob1.reset();
  EXPECT_FALSE(chainClone->isShared());

  // Coalesce the chain
  //
  // Coalescing this chain will create a new buffer and release the last
  // refcount on the original buffers we created.  Also make sure
  // that arrayBufFreeCount increases to one to indicate that arrayBuf was
  // freed.
  EXPECT_EQ(5, chainClone->countChainElements());
  EXPECT_EQ(0, arrayBufFreeCount);

  // Buffer lengths: 1500 20 1234 900 321
  // Coalesce the first 3 buffers
  chainClone->gather(1521);
  EXPECT_EQ(3, chainClone->countChainElements());
  EXPECT_EQ(0, arrayBufFreeCount);

  // Make sure the data is still the same after coalescing
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // Coalesce the entire chain
  chainClone->coalesce();
  EXPECT_EQ(1, chainClone->countChainElements());
  EXPECT_EQ(1, arrayBufFreeCount);

  // Make sure the data is still the same after coalescing
  EXPECT_EQ(fullLength, chainClone->computeChainDataLength());
  gen.seed(fillSeed);
  checkChain(chainClone.get(), gen);

  // Make a new chain to test the unlink and pop operations
  iob1 = IOBuf::create(1);
  iob1->append(1);
  IOBuf *iob1ptr = iob1.get();
  iob2 = IOBuf::create(3);
  iob2->append(3);
  iob2ptr = iob2.get();
  iob3 = IOBuf::create(5);
  iob3->append(5);
  iob3ptr = iob3.get();
  iob4 = IOBuf::create(7);
  iob4->append(7);
  iob4ptr = iob4.get();
  iob1->appendChain(std::move(iob2));
  iob1->prev()->appendChain(std::move(iob3));
  iob1->prev()->appendChain(std::move(iob4));
  EXPECT_EQ(4, iob1->countChainElements());
  EXPECT_EQ(16, iob1->computeChainDataLength());

  // Unlink from the middle of the chain
  iob3 = iob3ptr->unlink();
  EXPECT_TRUE(iob3.get() == iob3ptr);
  EXPECT_EQ(3, iob1->countChainElements());
  EXPECT_EQ(11, iob1->computeChainDataLength());

  // Unlink from the end of the chain
  iob4 = iob1->prev()->unlink();
  EXPECT_TRUE(iob4.get() == iob4ptr);
  EXPECT_EQ(2, iob1->countChainElements());
  EXPECT_TRUE(iob1->next() == iob2ptr);
  EXPECT_EQ(4, iob1->computeChainDataLength());

  // Pop from the front of the chain
  iob2 = iob1->pop();
  EXPECT_TRUE(iob1.get() == iob1ptr);
  EXPECT_EQ(1, iob1->countChainElements());
  EXPECT_EQ(1, iob1->computeChainDataLength());
  EXPECT_TRUE(iob2.get() == iob2ptr);
  EXPECT_EQ(1, iob2->countChainElements());
  EXPECT_EQ(3, iob2->computeChainDataLength());
}

TEST(IOBuf, Reserve) {
  uint32_t fillSeed = 0x23456789;
  boost::mt19937 gen(fillSeed);

  // Reserve does nothing if empty and doesn't have to grow the buffer
  {
    gen.seed(fillSeed);
    unique_ptr<IOBuf> iob(IOBuf::create(2000));
    EXPECT_EQ(0, iob->headroom());
    const void* p1 = iob->buffer();
    iob->reserve(5, 15);
    EXPECT_LE(5, iob->headroom());
    EXPECT_EQ(p1, iob->buffer());
  }

  // Reserve doesn't reallocate if we have enough total room
  {
    gen.seed(fillSeed);
    unique_ptr<IOBuf> iob(IOBuf::create(2000));
    iob->append(100);
    fillBuf(iob.get(), gen);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_EQ(100, iob->length());
    const void* p1 = iob->buffer();
    const uint8_t* d1 = iob->data();
    iob->reserve(100, 1800);
    EXPECT_LE(100, iob->headroom());
    EXPECT_EQ(p1, iob->buffer());
    EXPECT_EQ(d1 + 100, iob->data());
    gen.seed(fillSeed);
    checkBuf(iob.get(), gen);
  }

  // Reserve reallocates if we don't have enough total room.
  // NOTE that, with jemalloc, we know that this won't reallocate in place
  // as the size is less than jemallocMinInPlaceExpanadable
  {
    gen.seed(fillSeed);
    unique_ptr<IOBuf> iob(IOBuf::create(2000));
    iob->append(100);
    fillBuf(iob.get(), gen);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_EQ(100, iob->length());
    const void* p1 = iob->buffer();
    const uint8_t* d1 = iob->data();
    iob->reserve(100, 2512);  // allocation sizes are multiples of 256
    EXPECT_LE(100, iob->headroom());
    if (folly::usingJEMalloc()) {
      EXPECT_NE(p1, iob->buffer());
    }
    gen.seed(fillSeed);
    checkBuf(iob.get(), gen);
  }

  // Test reserve from internal buffer, this used to segfault
  {
    unique_ptr<IOBuf> iob(IOBuf::create(0));
    iob->reserve(0, 2000);
    EXPECT_EQ(0, iob->headroom());
    EXPECT_LE(2000, iob->tailroom());
  }
}

TEST(IOBuf, copyBuffer) {
  std::string s("hello");
  auto buf = IOBuf::copyBuffer(s.data(), s.size(), 1, 2);
  EXPECT_EQ(1, buf->headroom());
  EXPECT_EQ(s, std::string(reinterpret_cast<const char*>(buf->data()),
                           buf->length()));
  EXPECT_LE(2, buf->tailroom());

  buf = IOBuf::copyBuffer(s, 5, 7);
  EXPECT_EQ(5, buf->headroom());
  EXPECT_EQ(s, std::string(reinterpret_cast<const char*>(buf->data()),
                           buf->length()));
  EXPECT_LE(7, buf->tailroom());

  std::string empty;
  buf = IOBuf::copyBuffer(empty, 3, 6);
  EXPECT_EQ(3, buf->headroom());
  EXPECT_EQ(0, buf->length());
  EXPECT_LE(6, buf->tailroom());
}

TEST(IOBuf, maybeCopyBuffer) {
  std::string s("this is a test");
  auto buf = IOBuf::maybeCopyBuffer(s, 1, 2);
  EXPECT_EQ(1, buf->headroom());
  EXPECT_EQ(s, std::string(reinterpret_cast<const char*>(buf->data()),
                           buf->length()));
  EXPECT_LE(2, buf->tailroom());

  std::string empty;
  buf = IOBuf::maybeCopyBuffer("", 5, 7);
  EXPECT_EQ(nullptr, buf.get());

  buf = IOBuf::maybeCopyBuffer("");
  EXPECT_EQ(nullptr, buf.get());
}

namespace {

int customDeleterCount = 0;
int destructorCount = 0;
struct OwnershipTestClass {
  explicit OwnershipTestClass(int v = 0) : val(v) { }
  ~OwnershipTestClass() {
    ++destructorCount;
  }
  int val;
};

typedef std::function<void(OwnershipTestClass*)> CustomDeleter;

void customDelete(OwnershipTestClass* p) {
  ++customDeleterCount;
  delete p;
}

void customDeleteArray(OwnershipTestClass* p) {
  ++customDeleterCount;
  delete[] p;
}

}  // namespace

TEST(IOBuf, takeOwnershipUniquePtr) {
  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass> p(new OwnershipTestClass());
  }
  EXPECT_EQ(1, destructorCount);

  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass[]> p(new OwnershipTestClass[2]);
  }
  EXPECT_EQ(2, destructorCount);

  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass> p(new OwnershipTestClass());
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p)));
    EXPECT_EQ(sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(1, destructorCount);

  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass[]> p(new OwnershipTestClass[2]);
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p), 2));
    EXPECT_EQ(2 * sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(2, destructorCount);

  customDeleterCount = 0;
  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass, CustomDeleter>
      p(new OwnershipTestClass(), customDelete);
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p)));
    EXPECT_EQ(sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(1, destructorCount);
  EXPECT_EQ(1, customDeleterCount);

  customDeleterCount = 0;
  destructorCount = 0;
  {
    std::unique_ptr<OwnershipTestClass[], CustomDeleter>
      p(new OwnershipTestClass[2], customDeleteArray);
    std::unique_ptr<IOBuf> buf(IOBuf::takeOwnership(std::move(p), 2));
    EXPECT_EQ(2 * sizeof(OwnershipTestClass), buf->length());
    EXPECT_EQ(0, destructorCount);
  }
  EXPECT_EQ(2, destructorCount);
  EXPECT_EQ(1, customDeleterCount);
}

TEST(IOBuf, Alignment) {
  // max_align_t doesn't exist in gcc 4.6.2
  struct MaxAlign {
    char c;
  } __attribute__((aligned));
  size_t alignment = alignof(MaxAlign);

  std::vector<size_t> sizes {0, 1, 64, 256, 1024, 1 << 10};
  for (size_t size : sizes) {
    auto buf = IOBuf::create(size);
    uintptr_t p = reinterpret_cast<uintptr_t>(buf->data());
    EXPECT_EQ(0, p & (alignment - 1)) << "size=" << size;
  }
}

TEST(TypedIOBuf, Simple) {
  auto buf = IOBuf::create(0);
  TypedIOBuf<uint64_t> typed(buf.get());
  const uint64_t n = 10000;
  typed.reserve(0, n);
  EXPECT_LE(n, typed.capacity());
  for (uint64_t i = 0; i < n; i++) {
    *typed.writableTail() = i;
    typed.append(1);
  }
  EXPECT_EQ(n, typed.length());
  for (uint64_t i = 0; i < n; i++) {
    EXPECT_EQ(i, typed.data()[i]);
  }
}

// chain element size, number of elements in chain, shared
class MoveToFbStringTest
  : public ::testing::TestWithParam<std::tr1::tuple<int, int, bool>> {
 protected:
  void SetUp() {
    std::tr1::tie(elementSize_, elementCount_, shared_) = GetParam();
    buf_ = makeBuf();
    for (int i = 0; i < elementCount_ - 1; ++i) {
      buf_->prependChain(makeBuf());
    }
    EXPECT_EQ(elementCount_, buf_->countChainElements());
    EXPECT_EQ(elementCount_ * elementSize_, buf_->computeChainDataLength());
    if (shared_) {
      buf2_ = buf_->clone();
      EXPECT_EQ(elementCount_, buf2_->countChainElements());
      EXPECT_EQ(elementCount_ * elementSize_, buf2_->computeChainDataLength());
    }
  }

  std::unique_ptr<IOBuf> makeBuf() {
    auto buf = IOBuf::create(elementSize_);
    memset(buf->writableTail(), 'x', elementSize_);
    buf->append(elementSize_);
    return buf;
  }

  void check(std::unique_ptr<IOBuf>& buf) {
    fbstring str = buf->moveToFbString();
    EXPECT_EQ(elementCount_ * elementSize_, str.size());
    EXPECT_EQ(elementCount_ * elementSize_, strspn(str.c_str(), "x"));
    EXPECT_EQ(0, buf->length());
    EXPECT_EQ(1, buf->countChainElements());
    EXPECT_EQ(0, buf->computeChainDataLength());
    EXPECT_FALSE(buf->isChained());
  }

  int elementSize_;
  int elementCount_;
  bool shared_;
  std::unique_ptr<IOBuf> buf_;
  std::unique_ptr<IOBuf> buf2_;
};

TEST_P(MoveToFbStringTest, Simple) {
  check(buf_);
  if (shared_) {
    check(buf2_);
  }
}

INSTANTIATE_TEST_CASE_P(
    MoveToFbString,
    MoveToFbStringTest,
    ::testing::Combine(
        ::testing::Values(0, 1, 24, 256, 1 << 10, 1 << 20),  // element size
        ::testing::Values(1, 2, 10),                         // element count
        ::testing::Bool()));                                 // shared

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  google::ParseCommandLineFlags(&argc, &argv, true);

  return RUN_ALL_TESTS();
}
