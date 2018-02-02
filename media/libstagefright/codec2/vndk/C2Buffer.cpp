/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "C2Buffer"
#include <utils/Log.h>

#include <map>
#include <mutex>

#include <C2BufferPriv.h>
#include <C2BlockInternal.h>

namespace android {

namespace {

// This anonymous namespace contains the helper classes that allow our implementation to create
// block/buffer objects.
//
// Inherit from the parent, share with the friend.
class ReadViewBuddy : public C2ReadView {
    using C2ReadView::C2ReadView;
    friend class ::android::C2ConstLinearBlock;
};

class WriteViewBuddy : public C2WriteView {
    using C2WriteView::C2WriteView;
    friend class ::android::C2LinearBlock;
};

class ConstLinearBlockBuddy : public C2ConstLinearBlock {
    using C2ConstLinearBlock::C2ConstLinearBlock;
    friend class ::android::C2LinearBlock;
};

class LinearBlockBuddy : public C2LinearBlock {
    using C2LinearBlock::C2LinearBlock;
    friend class ::android::C2BasicLinearBlockPool;
};

class AcquirableReadViewBuddy : public C2Acquirable<C2ReadView> {
    using C2Acquirable::C2Acquirable;
    friend class ::android::C2ConstLinearBlock;
};

class AcquirableWriteViewBuddy : public C2Acquirable<C2WriteView> {
    using C2Acquirable::C2Acquirable;
    friend class ::android::C2LinearBlock;
};

class GraphicViewBuddy : public C2GraphicView {
    using C2GraphicView::C2GraphicView;
    friend class ::android::C2ConstGraphicBlock;
    friend class ::android::C2GraphicBlock;
};

class AcquirableConstGraphicViewBuddy : public C2Acquirable<const C2GraphicView> {
    using C2Acquirable::C2Acquirable;
    friend class ::android::C2ConstGraphicBlock;
};

class AcquirableGraphicViewBuddy : public C2Acquirable<C2GraphicView> {
    using C2Acquirable::C2Acquirable;
    friend class ::android::C2GraphicBlock;
};

class ConstGraphicBlockBuddy : public C2ConstGraphicBlock {
    using C2ConstGraphicBlock::C2ConstGraphicBlock;
    friend class ::android::C2GraphicBlock;
};

class GraphicBlockBuddy : public C2GraphicBlock {
    using C2GraphicBlock::C2GraphicBlock;
    friend class ::android::C2BasicGraphicBlockPool;
};

class BufferDataBuddy : public C2BufferData {
    using C2BufferData::C2BufferData;
    friend class ::android::C2Buffer;
};

}  // namespace

/* ========================================== 1D BLOCK ========================================= */

struct C2_HIDE _C2BlockPoolData;

/**
 * This class is the base class for all 1D block and view implementations.
 *
 * This is basically just a placeholder for the underlying 1D allocation and the range of the
 * alloted portion to this block. There is also a placeholder for a blockpool data.
 */
class C2_HIDE _C2Block1DImpl : public _C2LinearRangeAspect {
public:
    _C2Block1DImpl(const std::shared_ptr<C2LinearAllocation> &alloc,
            const std::shared_ptr<_C2BlockPoolData> &poolData = nullptr,
            size_t offset = 0, size_t size = ~(size_t)0)
        : _C2LinearRangeAspect(alloc.get(), offset, size),
          mAllocation(alloc),
          mPoolData(poolData) { }

    _C2Block1DImpl(const _C2Block1DImpl &other, size_t offset = 0, size_t size = ~(size_t)0)
        : _C2LinearRangeAspect(&other, offset, size),
          mAllocation(other.mAllocation),
          mPoolData(other.mPoolData) { }

    /** returns const pool data  */
    std::shared_ptr<const _C2BlockPoolData> poolData() const {
        return mPoolData;
    }

    /** returns native handle */
    const C2Handle *handle() const {
        return mAllocation ? mAllocation->handle() : nullptr;
    }

    /** returns the allocator's ID */
    C2Allocator::id_t getAllocatorId() const {
        // BAD_ID can only happen if this Impl class is initialized for a view - never for a block.
        return mAllocation ? mAllocation->getAllocatorId() : C2Allocator::BAD_ID;
    }

    std::shared_ptr<C2LinearAllocation> getAllocation() const {
        return mAllocation;
    }

private:
    std::shared_ptr<C2LinearAllocation> mAllocation;
    std::shared_ptr<_C2BlockPoolData> mPoolData;
};

/**
 * This class contains the mapped data pointer, and the potential error.
 *
 * range is the mapped range of the underlying allocation (which is part of the allotted
 * range).
 */
class C2_HIDE _C2MappedBlock1DImpl : public _C2Block1DImpl {
public:
    _C2MappedBlock1DImpl(const _C2Block1DImpl &block, uint8_t *data,
                         size_t offset = 0, size_t size = ~(size_t)0)
        : _C2Block1DImpl(block, offset, size), mData(data), mError(C2_OK) { }

    _C2MappedBlock1DImpl(c2_status_t error)
        : _C2Block1DImpl(nullptr), mData(nullptr), mError(error) {
        // CHECK(error != C2_OK);
    }

    const uint8_t *data() const {
        return mData;
    }

    uint8_t *data() {
        return mData;
    }

    c2_status_t error() const {
        return mError;
    }

private:
    uint8_t *mData;
    c2_status_t mError;
};

/**
 * Block implementation.
 */
class C2Block1D::Impl : public _C2Block1DImpl {
    using _C2Block1DImpl::_C2Block1DImpl;
};

const C2Handle *C2Block1D::handle() const {
    return mImpl->handle();
};

C2Allocator::id_t C2Block1D::getAllocatorId() const {
    return mImpl->getAllocatorId();
};

C2Block1D::C2Block1D(std::shared_ptr<Impl> impl, const _C2LinearRangeAspect &range)
    // always clamp subrange to parent (impl) range for safety
    : _C2LinearRangeAspect(impl.get(), range.offset(), range.size()), mImpl(impl) {
}

/**
 * Read view implementation.
 *
 * range of Impl is the mapped range of the underlying allocation (which is part of the allotted
 * range). range of View is 0 to capacity() (not represented as an actual range). This maps to a
 * subrange of Impl range starting at mImpl->offset() + _mOffset.
 */
class C2ReadView::Impl : public _C2MappedBlock1DImpl {
    using _C2MappedBlock1DImpl::_C2MappedBlock1DImpl;
};

C2ReadView::C2ReadView(std::shared_ptr<Impl> impl, uint32_t offset, uint32_t size)
    : _C2LinearCapacityAspect(C2LinearCapacity(impl->size()).range(offset, size).size()),
      mImpl(impl),
      mOffset(C2LinearCapacity(impl->size()).range(offset, size).offset()) { }

C2ReadView::C2ReadView(c2_status_t error)
    : _C2LinearCapacityAspect(0u), mImpl(std::make_shared<Impl>(error)), mOffset(0u) {
    // CHECK(error != C2_OK);
}

const uint8_t *C2ReadView::data() const {
    return mImpl->error() ? nullptr : mImpl->data() + mOffset;
}

c2_status_t C2ReadView::error() const {
    return mImpl->error();
}

C2ReadView C2ReadView::subView(size_t offset, size_t size) const {
    C2LinearRange subRange(*this, offset, size);
    return C2ReadView(mImpl, mOffset + subRange.offset(), subRange.size());
}

/**
 * Write view implementation.
 */
class C2WriteView::Impl : public _C2MappedBlock1DImpl {
    using _C2MappedBlock1DImpl::_C2MappedBlock1DImpl;
};

C2WriteView::C2WriteView(std::shared_ptr<Impl> impl)
// UGLY: _C2LinearRangeAspect requires a bona-fide object for capacity to prevent spoofing, so
// this is what we have to do.
// TODO: use childRange
    : _C2EditableLinearRangeAspect(std::make_unique<C2LinearCapacity>(impl->size()).get()), mImpl(impl) { }

C2WriteView::C2WriteView(c2_status_t error)
    : _C2EditableLinearRangeAspect(nullptr), mImpl(std::make_shared<Impl>(error)) {}

uint8_t *C2WriteView::base() { return mImpl->data(); }

uint8_t *C2WriteView::data() { return mImpl->data() + offset(); }

c2_status_t C2WriteView::error() const { return mImpl->error(); }

/**
 * Const linear block implementation.
 */
C2ConstLinearBlock::C2ConstLinearBlock(std::shared_ptr<Impl> impl, const _C2LinearRangeAspect &range, C2Fence fence)
    : C2Block1D(impl, range), mFence(fence) { }

C2Acquirable<C2ReadView> C2ConstLinearBlock::map() const {
    void *base = nullptr;
    uint32_t len = size();
    c2_status_t error = mImpl->getAllocation()->map(
            offset(), len, { C2MemoryUsage::CPU_READ, 0 }, nullptr, &base);
    // TODO: wait on fence
    if (error == C2_OK) {
        std::shared_ptr<ReadViewBuddy::Impl> rvi = std::shared_ptr<ReadViewBuddy::Impl>(
                new ReadViewBuddy::Impl(*mImpl, (uint8_t *)base, offset(), len),
                [base, len](ReadViewBuddy::Impl *i) {
                    (void)i->getAllocation()->unmap(base, len, nullptr);
        });
        return AcquirableReadViewBuddy(error, C2Fence(), ReadViewBuddy(rvi, 0, len));
    } else {
        return AcquirableReadViewBuddy(error, C2Fence(), ReadViewBuddy(error));
    }
}

C2ConstLinearBlock C2ConstLinearBlock::subBlock(size_t offset_, size_t size_) const {
    C2LinearRange subRange(*mImpl, offset_, size_);
    return C2ConstLinearBlock(mImpl, subRange, mFence);
}

/**
 * Linear block implementation.
 */
C2LinearBlock::C2LinearBlock(std::shared_ptr<Impl> impl, const _C2LinearRangeAspect &range)
    : C2Block1D(impl, range) { }

C2Acquirable<C2WriteView> C2LinearBlock::map() {
    void *base = nullptr;
    uint32_t len = size();
    c2_status_t error = mImpl->getAllocation()->map(
            offset(), len, { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, nullptr, &base);
    // TODO: wait on fence
    if (error == C2_OK) {
        std::shared_ptr<WriteViewBuddy::Impl> rvi = std::shared_ptr<WriteViewBuddy::Impl>(
                new WriteViewBuddy::Impl(*mImpl, (uint8_t *)base, 0, len),
                [base, len](WriteViewBuddy::Impl *i) {
                    (void)i->getAllocation()->unmap(base, len, nullptr);
        });
        return AcquirableWriteViewBuddy(error, C2Fence(), WriteViewBuddy(rvi));
    } else {
        return AcquirableWriteViewBuddy(error, C2Fence(), WriteViewBuddy(error));
    }
}

C2ConstLinearBlock C2LinearBlock::share(size_t offset_, size_t size_, C2Fence fence) {
    return ConstLinearBlockBuddy(mImpl, C2LinearRange(*this, offset_, size_), fence);
}

C2BasicLinearBlockPool::C2BasicLinearBlockPool(
        const std::shared_ptr<C2Allocator> &allocator)
  : mAllocator(allocator) { }

c2_status_t C2BasicLinearBlockPool::fetchLinearBlock(
        uint32_t capacity,
        C2MemoryUsage usage,
        std::shared_ptr<C2LinearBlock> *block /* nonnull */) {
    block->reset();

    std::shared_ptr<C2LinearAllocation> alloc;
    c2_status_t err = mAllocator->newLinearAllocation(capacity, usage, &alloc);
    if (err != C2_OK) {
        return err;
    }

    *block = _C2BlockFactory::CreateLinearBlock(alloc);

    return C2_OK;
}

std::shared_ptr<C2LinearBlock> _C2BlockFactory::CreateLinearBlock(
        const std::shared_ptr<C2LinearAllocation> &alloc,
        const std::shared_ptr<_C2BlockPoolData> &data, size_t offset, size_t size) {
    std::shared_ptr<C2Block1D::Impl> impl =
        std::make_shared<C2Block1D::Impl>(alloc, data, offset, size);
    return std::shared_ptr<C2LinearBlock>(new C2LinearBlock(impl, *impl));
}

/* ========================================== 2D BLOCK ========================================= */

/**
 * Implementation that is shared between all 2D blocks and views.
 *
 * For blocks' Impl's crop is always the allotted crop, even if it is a sub block.
 *
 * For views' Impl's crop is the mapped portion - which for now is always the
 * allotted crop.
 */
class C2_HIDE _C2Block2DImpl : public _C2PlanarSectionAspect {
public:
    /**
     * Impl's crop is always the or part of the allotted crop of the allocation.
     */
    _C2Block2DImpl(const std::shared_ptr<C2GraphicAllocation> &alloc,
            const std::shared_ptr<_C2BlockPoolData> &poolData = nullptr,
            const C2Rect &allottedCrop = C2Rect(~0u, ~0u))
        : _C2PlanarSectionAspect(alloc.get(), allottedCrop),
          mAllocation(alloc),
          mPoolData(poolData) { }

    /** returns const pool data  */
    std::shared_ptr<const _C2BlockPoolData> poolData() const {
        return mPoolData;
    }

    /** returns native handle */
    const C2Handle *handle() const {
        return mAllocation ? mAllocation->handle() : nullptr;
    }

    /** returns the allocator's ID */
    C2Allocator::id_t getAllocatorId() const {
        // BAD_ID can only happen if this Impl class is initialized for a view - never for a block.
        return mAllocation ? mAllocation->getAllocatorId() : C2Allocator::BAD_ID;
    }

    std::shared_ptr<C2GraphicAllocation> getAllocation() const {
        return mAllocation;
    }

private:
    std::shared_ptr<C2GraphicAllocation> mAllocation;
    std::shared_ptr<_C2BlockPoolData> mPoolData;
};

class C2_HIDE _C2MappingBlock2DImpl : public _C2Block2DImpl {
public:
    using _C2Block2DImpl::_C2Block2DImpl;

    /**
     * This class contains the mapped data pointer, and the potential error.
     */
    struct Mapped {
    private:
        friend class _C2MappingBlock2DImpl;

        Mapped(const _C2Block2DImpl *impl, bool writable, C2Fence *fence __unused)
            : mImpl(impl), mWritable(writable) {
            memset(mData, 0, sizeof(mData));
            const C2Rect crop = mImpl->crop();
            // gralloc requires mapping the whole region of interest as we cannot
            // map multiple regions
            mError = mImpl->getAllocation()->map(
                    crop,
                    { C2MemoryUsage::CPU_READ, writable ? C2MemoryUsage::CPU_WRITE : 0 },
                    nullptr,
                    &mLayout,
                    mData);
            if (mError != C2_OK) {
                memset(&mLayout, 0, sizeof(mLayout));
                memset(mData, 0, sizeof(mData));
            } else {
                // TODO: validate plane layout and
                // adjust data pointers to the crop region's top left corner.
                // fail if it is not on a subsampling boundary
                for (size_t planeIx = 0; planeIx < mLayout.numPlanes; ++planeIx) {
                    const uint32_t colSampling = mLayout.planes[planeIx].colSampling;
                    const uint32_t rowSampling = mLayout.planes[planeIx].rowSampling;
                    if (crop.left % colSampling || crop.right() % colSampling
                            || crop.top % rowSampling || crop.bottom() % rowSampling) {
                        // cannot calculate data pointer
                        mImpl->getAllocation()->unmap(nullptr);
                        memset(&mLayout, 0, sizeof(mLayout));
                        memset(mData, 0, sizeof(mData));
                        mError = C2_BAD_VALUE;
                        return;
                    }
                    mData[planeIx] += (ssize_t)crop.left * mLayout.planes[planeIx].colInc
                            + (ssize_t)crop.top * mLayout.planes[planeIx].rowInc;
                }
            }
        }

        explicit Mapped(c2_status_t error)
            : mImpl(nullptr), mWritable(false), mError(error) {
            // CHECK(error != C2_OK);
            memset(&mLayout, 0, sizeof(mLayout));
            memset(mData, 0, sizeof(mData));
        }

    public:
        ~Mapped() {
            if (mData[0] != nullptr) {
                mImpl->getAllocation()->unmap(nullptr);
            }
        }

        /** returns mapping status */
        c2_status_t error() const { return mError; }

        /** returns data pointer */
        uint8_t *const *data() const { return mData; }

        /** returns the plane layout */
        C2PlanarLayout layout() const { return mLayout; }

        /** returns whether the mapping is writable */
        bool writable() const { return mWritable; }

    private:
        const _C2Block2DImpl *mImpl;
        bool mWritable;
        c2_status_t mError;
        uint8_t *mData[C2PlanarLayout::MAX_NUM_PLANES];
        C2PlanarLayout mLayout;
    };

    /**
     * Maps the allotted region.
     *
     * If already mapped and it is currently in use, returns the existing mapping.
     * If fence is provided, an acquire fence is stored there.
     */
    std::shared_ptr<Mapped> map(bool writable, C2Fence *fence) {
        std::lock_guard<std::mutex> lock(mMappedLock);
        std::shared_ptr<Mapped> existing = mMapped.lock();
        if (!existing) {
            existing = std::shared_ptr<Mapped>(new Mapped(this, writable, fence));
            mMapped = existing;
        } else {
            // if we mapped the region read-only, we cannot remap it read-write
            if (writable && !existing->writable()) {
                existing = std::shared_ptr<Mapped>(new Mapped(C2_CANNOT_DO));
            }
            if (fence != nullptr) {
                *fence = C2Fence();
            }
        }
        return existing;
    }

private:
    std::weak_ptr<Mapped> mMapped;
    std::mutex mMappedLock;
};

class C2_HIDE _C2MappedBlock2DImpl : public _C2Block2DImpl {
public:
    _C2MappedBlock2DImpl(const _C2Block2DImpl &impl,
                         std::shared_ptr<_C2MappingBlock2DImpl::Mapped> mapping)
        : _C2Block2DImpl(impl), mMapping(mapping) {
    }

    std::shared_ptr<_C2MappingBlock2DImpl::Mapped> mapping() const { return mMapping; }

private:
    std::shared_ptr<_C2MappingBlock2DImpl::Mapped> mMapping;
};

/**
 * Block implementation.
 */
class C2Block2D::Impl : public _C2MappingBlock2DImpl {
    using _C2MappingBlock2DImpl::_C2MappingBlock2DImpl;
};

const C2Handle *C2Block2D::handle() const {
    return mImpl->handle();
}

C2Allocator::id_t C2Block2D::getAllocatorId() const {
    return mImpl->getAllocatorId();
}

C2Block2D::C2Block2D(std::shared_ptr<Impl> impl, const _C2PlanarSectionAspect &section)
    // always clamp subsection to parent (impl) crop for safety
    : _C2PlanarSectionAspect(impl.get(), section.crop()), mImpl(impl) {
}

/**
 * Graphic view implementation.
 *
 * range of Impl is the mapped range of the underlying allocation. range of View is the current
 * crop.
 */
class C2GraphicView::Impl : public _C2MappedBlock2DImpl {
    using _C2MappedBlock2DImpl::_C2MappedBlock2DImpl;
};

C2GraphicView::C2GraphicView(std::shared_ptr<Impl> impl, const _C2PlanarSectionAspect &section)
    : _C2EditablePlanarSectionAspect(impl.get(), section.crop()), mImpl(impl) {
}

const uint8_t *const *C2GraphicView::data() const {
    return mImpl->mapping()->data();
}

uint8_t *const *C2GraphicView::data() {
    return mImpl->mapping()->data();
}

const C2PlanarLayout C2GraphicView::layout() const {
    return mImpl->mapping()->layout();
}

const C2GraphicView C2GraphicView::subView(const C2Rect &rect) const {
    return C2GraphicView(mImpl, C2PlanarSection(*mImpl, rect));
}

C2GraphicView C2GraphicView::subView(const C2Rect &rect) {
    return C2GraphicView(mImpl, C2PlanarSection(*mImpl, rect));
}

c2_status_t C2GraphicView::error() const {
    return mImpl->mapping()->error();
}

/**
 * Const graphic block implementation.
 */
C2ConstGraphicBlock::C2ConstGraphicBlock(
        std::shared_ptr<Impl> impl, const _C2PlanarSectionAspect &section, C2Fence fence)
    : C2Block2D(impl, section), mFence(fence) { }

C2Acquirable<const C2GraphicView> C2ConstGraphicBlock::map() const {
    C2Fence fence;
    std::shared_ptr<_C2MappingBlock2DImpl::Mapped> mapping =
        mImpl->map(false /* writable */, &fence);
    std::shared_ptr<GraphicViewBuddy::Impl> gvi =
        std::shared_ptr<GraphicViewBuddy::Impl>(new GraphicViewBuddy::Impl(*mImpl, mapping));
    return AcquirableConstGraphicViewBuddy(
            mapping->error(), fence, GraphicViewBuddy(gvi, C2PlanarSection(*mImpl, crop())));
}

C2ConstGraphicBlock C2ConstGraphicBlock::subBlock(const C2Rect &rect) const {
    return C2ConstGraphicBlock(mImpl, C2PlanarSection(*mImpl, crop().intersect(rect)), mFence);
}

/**
 * Graphic block implementation.
 */
C2GraphicBlock::C2GraphicBlock(
    std::shared_ptr<Impl> impl, const _C2PlanarSectionAspect &section)
    : C2Block2D(impl, section) { }

C2Acquirable<C2GraphicView> C2GraphicBlock::map() {
    C2Fence fence;
    std::shared_ptr<_C2MappingBlock2DImpl::Mapped> mapping =
        mImpl->map(true /* writable */, &fence);
    std::shared_ptr<GraphicViewBuddy::Impl> gvi =
        std::shared_ptr<GraphicViewBuddy::Impl>(new GraphicViewBuddy::Impl(*mImpl, mapping));
    return AcquirableGraphicViewBuddy(
            mapping->error(), fence, GraphicViewBuddy(gvi, C2PlanarSection(*mImpl, crop())));
}

C2ConstGraphicBlock C2GraphicBlock::share(const C2Rect &crop, C2Fence fence) {
    return ConstGraphicBlockBuddy(mImpl, C2PlanarSection(*mImpl, crop), fence);
}

/**
 * Basic block pool implementations.
 */
C2BasicGraphicBlockPool::C2BasicGraphicBlockPool(
        const std::shared_ptr<C2Allocator> &allocator)
  : mAllocator(allocator) {}

c2_status_t C2BasicGraphicBlockPool::fetchGraphicBlock(
        uint32_t width,
        uint32_t height,
        uint32_t format,
        C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock> *block /* nonnull */) {
    block->reset();

    std::shared_ptr<C2GraphicAllocation> alloc;
    c2_status_t err = mAllocator->newGraphicAllocation(width, height, format, usage, &alloc);
    if (err != C2_OK) {
        return err;
    }

    *block = _C2BlockFactory::CreateGraphicBlock(alloc);

    return C2_OK;
}

std::shared_ptr<C2GraphicBlock> _C2BlockFactory::CreateGraphicBlock(
        const std::shared_ptr<C2GraphicAllocation> &alloc,
        const std::shared_ptr<_C2BlockPoolData> &data, const C2Rect &allottedCrop) {
    std::shared_ptr<C2Block2D::Impl> impl =
        std::make_shared<C2Block2D::Impl>(alloc, data, allottedCrop);
    return std::shared_ptr<C2GraphicBlock>(new C2GraphicBlock(impl, *impl));
}

/* ========================================== BUFFER ========================================= */

class C2BufferData::Impl {
public:
    explicit Impl(const std::vector<C2ConstLinearBlock> &blocks)
        : mType(blocks.size() == 1 ? LINEAR : LINEAR_CHUNKS),
          mLinearBlocks(blocks) {
    }

    explicit Impl(const std::vector<C2ConstGraphicBlock> &blocks)
        : mType(blocks.size() == 1 ? GRAPHIC : GRAPHIC_CHUNKS),
          mGraphicBlocks(blocks) {
    }

    Type type() const { return mType; }
    const std::vector<C2ConstLinearBlock> &linearBlocks() const { return mLinearBlocks; }
    const std::vector<C2ConstGraphicBlock> &graphicBlocks() const { return mGraphicBlocks; }

private:
    Type mType;
    std::vector<C2ConstLinearBlock> mLinearBlocks;
    std::vector<C2ConstGraphicBlock> mGraphicBlocks;
};

C2BufferData::C2BufferData(const std::vector<C2ConstLinearBlock> &blocks) : mImpl(new Impl(blocks)) {}
C2BufferData::C2BufferData(const std::vector<C2ConstGraphicBlock> &blocks) : mImpl(new Impl(blocks)) {}

C2BufferData::Type C2BufferData::type() const { return mImpl->type(); }

const std::vector<C2ConstLinearBlock> C2BufferData::linearBlocks() const {
    return mImpl->linearBlocks();
}

const std::vector<C2ConstGraphicBlock> C2BufferData::graphicBlocks() const {
    return mImpl->graphicBlocks();
}

class C2Buffer::Impl {
public:
    Impl(C2Buffer *thiz, const std::vector<C2ConstLinearBlock> &blocks)
        : mThis(thiz), mData(blocks) {}
    Impl(C2Buffer *thiz, const std::vector<C2ConstGraphicBlock> &blocks)
        : mThis(thiz), mData(blocks) {}

    ~Impl() {
        for (const auto &pair : mNotify) {
            pair.first(mThis, pair.second);
        }
    }

    const C2BufferData &data() const { return mData; }

    c2_status_t registerOnDestroyNotify(OnDestroyNotify onDestroyNotify, void *arg) {
        auto it = std::find_if(
                mNotify.begin(), mNotify.end(),
                [onDestroyNotify, arg] (const auto &pair) {
                    return pair.first == onDestroyNotify && pair.second == arg;
                });
        if (it != mNotify.end()) {
            return C2_DUPLICATE;
        }
        mNotify.emplace_back(onDestroyNotify, arg);
        return C2_OK;
    }

    c2_status_t unregisterOnDestroyNotify(OnDestroyNotify onDestroyNotify, void *arg) {
        auto it = std::find_if(
                mNotify.begin(), mNotify.end(),
                [onDestroyNotify, arg] (const auto &pair) {
                    return pair.first == onDestroyNotify && pair.second == arg;
                });
        if (it == mNotify.end()) {
            return C2_NOT_FOUND;
        }
        mNotify.erase(it);
        return C2_OK;
    }

    std::vector<std::shared_ptr<const C2Info>> info() const {
        std::vector<std::shared_ptr<const C2Info>> result(mInfos.size());
        std::transform(
                mInfos.begin(), mInfos.end(), result.begin(),
                [] (const auto &elem) { return elem.second; });
        return result;
    }

    c2_status_t setInfo(const std::shared_ptr<C2Info> &info) {
        // To "update" you need to erase the existing one if any, and then insert.
        (void) mInfos.erase(info->type());
        (void) mInfos.insert({ info->type(), info });
        return C2_OK;
    }

    bool hasInfo(C2Param::Type index) const {
        return mInfos.count(index.type()) > 0;
    }

    std::shared_ptr<C2Info> removeInfo(C2Param::Type index) {
        auto it = mInfos.find(index.type());
        if (it == mInfos.end()) {
            return nullptr;
        }
        std::shared_ptr<C2Info> ret = it->second;
        (void) mInfos.erase(it);
        return ret;
    }

private:
    C2Buffer * const mThis;
    BufferDataBuddy mData;
    std::map<C2Param::Type, std::shared_ptr<C2Info>> mInfos;
    std::list<std::pair<OnDestroyNotify, void *>> mNotify;
};

C2Buffer::C2Buffer(const std::vector<C2ConstLinearBlock> &blocks)
    : mImpl(new Impl(this, blocks)) {}

C2Buffer::C2Buffer(const std::vector<C2ConstGraphicBlock> &blocks)
    : mImpl(new Impl(this, blocks)) {}

const C2BufferData C2Buffer::data() const { return mImpl->data(); }

c2_status_t C2Buffer::registerOnDestroyNotify(OnDestroyNotify onDestroyNotify, void *arg) {
    return mImpl->registerOnDestroyNotify(onDestroyNotify, arg);
}

c2_status_t C2Buffer::unregisterOnDestroyNotify(OnDestroyNotify onDestroyNotify, void *arg) {
    return mImpl->unregisterOnDestroyNotify(onDestroyNotify, arg);
}

const std::vector<std::shared_ptr<const C2Info>> C2Buffer::info() const {
    return mImpl->info();
}

c2_status_t C2Buffer::setInfo(const std::shared_ptr<C2Info> &info) {
    return mImpl->setInfo(info);
}

bool C2Buffer::hasInfo(C2Param::Type index) const {
    return mImpl->hasInfo(index);
}

std::shared_ptr<C2Info> C2Buffer::removeInfo(C2Param::Type index) {
    return mImpl->removeInfo(index);
}

} // namespace android
