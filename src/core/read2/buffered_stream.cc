//------------------------------------------------------------------------------
// Copyright 2021 H2O.ai
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//------------------------------------------------------------------------------
#include <deque>
#include "read2/buffered_stream.h"
#include "read2/stream.h"
#include "utils/assert.h"
namespace dt {
namespace read2 {


//------------------------------------------------------------------------------
// BufferedStream_Buffer
//------------------------------------------------------------------------------

class BufferedStream_Buffer : public BufferedStream {
  private:
    Buffer buffer_;

  public:
    BufferedStream_Buffer(Buffer&& buf)
      : buffer_(std::move(buf)) {}


    Buffer getChunk(size_t start, size_t size) override {
      size_t n = buffer_.size();
      if (start > n) size = 0;
      else if (start + size > n) size = n - start;
      return Buffer::view(buffer_, size, start);
    }

    void stream() override {}
    void releaseChunk(size_t) override {}

    Buffer readChunk(size_t) override {
      Buffer res = std::move(buffer_);
      xassert(!buffer_);
      return res;
    }
};




//------------------------------------------------------------------------------
// BufferedStream_Stream
//------------------------------------------------------------------------------

class BufferedStream_Stream : public BufferedStream {
  private:
    static constexpr size_t ALL = size_t(-1);
    struct Piece {
      size_t offset0;
      size_t offset1;
      Buffer buffer;
    };
    std::unique_ptr<Stream> stream_;
    std::deque<Piece> pieces_;
    size_t nPiecesRead_;
    size_t nBytesRead_;
    size_t memoryLimit_;
    size_t memoryUsed_;
    std::mutex mutex_;
    std::condition_variable cv_;

  public:
    BufferedStream_Stream(std::unique_ptr<Stream>&& stream, size_t memoryLimit)
      : stream_(std::move(stream)),
        nPiecesRead_(0),
        nBytesRead_(0),
        memoryLimit_(memoryLimit),
        memoryUsed_(0)
    {
      xassert(memoryLimit_ > 0);
    }


    Buffer getChunk(size_t start, size_t size) override {
      xassert(size > 0);
      xassert(pieces_.empty() || start >= pieces_.front().offset0);
      while (true) {
        std::vector<Buffer> fragments;
        size_t remainingSize = size;
        size_t nPiecesRead;
        {
          // pieces_ array must be read under the protection of a mutex because
          // it is modified in `stream()` and `releaseChunk()`, which could be
          // invoked from other threads.
          std::lock_guard<std::mutex> lock(mutex_);
          for (const auto& piece : pieces_) {
            const size_t offset0 = piece.offset0;
            const size_t offset1 = piece.offset1;
            if (start >= offset1) continue;
            size_t fragmentStart = (start > offset0)? start - offset0 : 0;
            size_t fragmentSize = std::min(remainingSize,
                                           offset1 - offset0 - fragmentStart);
            fragments.push_back(
                Buffer::view(piece.buffer, fragmentSize, fragmentStart));
            remainingSize -= fragmentSize;
            if (remainingSize == 0) break;
          }
          nPiecesRead = nPiecesRead_;
        }  // `mutex_` unlocked
        if (remainingSize == 0 || nPiecesRead == ALL) {
          return concatenateBuffers(fragments);
        }
        // otherwise, not all required pieces have been read yet -- need to
        // wait until more data becomes available.
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [&]{ return nPiecesRead_ > nPiecesRead; });
        }
      }
    }

    void stream() override {
      bool done = false;
      while (!done) {
        if (memoryUsed_ > memoryLimit_) {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [&]{ return memoryUsed_ < memoryLimit_; });
        }
        auto buffer = stream_->readChunk(1024*1024);
        auto size = buffer.size();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (size == 0) {
            nPiecesRead_ = ALL;
            done = true;
          } else {
            pieces_.push_back({ nBytesRead_, nBytesRead_ + size, buffer });
            nPiecesRead_++;
            nBytesRead_ += size;
            memoryUsed_ += size;
          }
        }
        cv_.notify_all();
      }
    }

    void releaseChunk(size_t upTo) override {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& piece : pieces_) {
          if (piece.offset1 > upTo) break;
          pieces_.pop_front();
        }
      }
      cv_.notify_all();
    }

    Buffer readChunk(size_t requestedSize) override {
      if (!pieces_.empty()) {
        Buffer res = std::move(pieces_.front().buffer);
        pieces_.pop_front();
        return res;
      }
      if (nPiecesRead_ == ALL) {
        return Buffer();
      } else {
        Buffer buf = stream_->readChunk(requestedSize);
        if (!buf) {
          nPiecesRead_ = ALL;
        }
        return buf;
      }
    }

  private:
    Buffer concatenateBuffers(const std::vector<Buffer>& buffers) {
      if (buffers.size() == 0) {
        return Buffer();
      }
      if (buffers.size() == 1) {
        return buffers[0];
      }
      size_t total = 0;
      for (const auto& buf : buffers) {
        total += buf.size();
      }
      Buffer out = Buffer::mem(total);
      char* outPtr = static_cast<char*>(out.xptr());
      for (const auto& buf : buffers) {
        std::memcpy(outPtr, buf.rptr(), buf.size());
        outPtr += buf.size();
      }
      return out;
    }
};




//------------------------------------------------------------------------------
// BufferedStream
//------------------------------------------------------------------------------
using BufferedStreamPtr = std::unique_ptr<BufferedStream>;

BufferedStream::~BufferedStream() {}


BufferedStreamPtr BufferedStream::fromBuffer(Buffer buf) {
  return BufferedStreamPtr(new BufferedStream_Buffer(std::move(buf)));
}


BufferedStreamPtr BufferedStream::fromStream(
    std::unique_ptr<Stream>&& stream, size_t memoryLimit)
{
  return BufferedStreamPtr(
      new BufferedStream_Stream(std::move(stream), memoryLimit));
}




}}  // namespace dt::read2