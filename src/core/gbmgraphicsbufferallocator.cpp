/*
    SPDX-FileCopyrightText: 2023 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "core/gbmgraphicsbufferallocator.h"

#include "config-kwin.h"

#include "core/graphicsbuffer.h"
#include "utils/common.h"
#include "utils/memorymap.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#if defined(Q_OS_LINUX)
#include <linux/udmabuf.h>
#endif

namespace KWin
{

static inline std::optional<DmaBufAttributes> dmaBufAttributesForBo(gbm_bo *bo, dev_t deviceId)
{
    DmaBufAttributes attributes;
    attributes.planeCount = gbm_bo_get_plane_count(bo);
    attributes.width = gbm_bo_get_width(bo);
    attributes.height = gbm_bo_get_height(bo);
    attributes.format = gbm_bo_get_format(bo);
    attributes.modifier = gbm_bo_get_modifier(bo);
    attributes.device = deviceId;

#if HAVE_GBM_BO_GET_FD_FOR_PLANE
    for (int i = 0; i < attributes.planeCount; ++i) {
        attributes.fd[i] = FileDescriptor{gbm_bo_get_fd_for_plane(bo, i)};
        if (!attributes.fd[i].isValid()) {
            qWarning() << "gbm_bo_get_fd_for_plane() failed:" << strerror(errno);
            return std::nullopt;
        }
        attributes.offset[i] = gbm_bo_get_offset(bo, i);
        attributes.pitch[i] = gbm_bo_get_stride_for_plane(bo, i);
    }
#else
    if (attributes.planeCount > 1) {
        return attributes;
    }

    attributes.fd[0] = FileDescriptor{gbm_bo_get_fd(bo)};
    if (!attributes.fd[0].isValid()) {
        qWarning() << "gbm_bo_get_fd() failed:" << strerror(errno);
        return std::nullopt;
    }
    attributes.offset[0] = gbm_bo_get_offset(bo, 0);
    attributes.pitch[0] = gbm_bo_get_stride_for_plane(bo, 0);
#endif

    return attributes;
}

class GbmGraphicsBuffer : public GraphicsBuffer
{
    Q_OBJECT

public:
    GbmGraphicsBuffer(DmaBufAttributes attributes, gbm_bo *handle);
    ~GbmGraphicsBuffer() override;

    Map map(MapFlags flags) override;
    void unmap() override;

    QSize size() const override;
    bool hasAlphaChannel() const override;
    const DmaBufAttributes *dmabufAttributes() const override;

private:
    gbm_bo *m_bo;
    void *m_mapPtr = nullptr;
    void *m_mapData = nullptr;
    // the stride of the buffer mapping can be different from the stride of the buffer itself
    uint32_t m_mapStride = 0;
    DmaBufAttributes m_dmabufAttributes;
    QSize m_size;
    bool m_hasAlphaChannel;
};

class DumbGraphicsBuffer : public GraphicsBuffer
{
    Q_OBJECT

public:
    DumbGraphicsBuffer(int drmFd, uint32_t handle, DmaBufAttributes attributes);
    ~DumbGraphicsBuffer() override;

    Map map(MapFlags flags) override;
    void unmap() override;

    QSize size() const override;
    bool hasAlphaChannel() const override;
    const DmaBufAttributes *dmabufAttributes() const override;

private:
    int m_drmFd;
    uint32_t m_handle;
    void *m_data = nullptr;
    size_t m_size = 0;
    DmaBufAttributes m_dmabufAttributes;
    bool m_hasAlphaChannel;
};

class UDmabufGraphicsBuffer : public GraphicsBuffer
{
    Q_OBJECT

public:
    explicit UDmabufGraphicsBuffer(DmaBufAttributes &&attributes, MemoryMap &&memoryMap);

    Map map(MapFlags flags) override;
    void unmap() override;

    QSize size() const override;
    bool hasAlphaChannel() const override;
    const DmaBufAttributes *dmabufAttributes() const override;

private:
    DmaBufAttributes m_attributes;
    MemoryMap m_memoryMap;
    bool m_hasAlphaChannel;
};

UDmabufGraphicsBuffer::UDmabufGraphicsBuffer(DmaBufAttributes &&attributes, MemoryMap &&memoryMap)
    : m_attributes(std::move(attributes))
    , m_memoryMap(std::move(memoryMap))
    , m_hasAlphaChannel(alphaChannelFromDrmFormat(attributes.format))
{
}

GraphicsBuffer::Map UDmabufGraphicsBuffer::map(MapFlags flags)
{
    if (m_memoryMap.isValid()) {
        return Map{
            .data = m_memoryMap.data(),
            .stride = uint32_t(m_attributes.pitch[0]),
        };
    } else {
        return Map{};
    }
}

void UDmabufGraphicsBuffer::unmap()
{
}

QSize UDmabufGraphicsBuffer::size() const
{
    return QSize(m_attributes.width, m_attributes.height);
}

bool UDmabufGraphicsBuffer::hasAlphaChannel() const
{
    return m_hasAlphaChannel;
}

const DmaBufAttributes *UDmabufGraphicsBuffer::dmabufAttributes() const
{
    return &m_attributes;
}

GraphicsBuffer *allocateUdmabuf(const FileDescriptor &dev, dev_t devNum, const GraphicsBufferOptions &options)
{
#if defined(Q_OS_LINUX)
    if (!options.software) {
        return nullptr;
    }
    if (!options.modifiers.empty() && !options.modifiers.contains(DRM_FORMAT_MOD_LINEAR)) {
        return nullptr;
    }

    const auto info = FormatInfo::get(options.format);
    if (!info || (info->bitsPerPixel % 8 != 0)) {
        return nullptr;
    }
    const uint32_t bytesPerPixel = info->bitsPerPixel / 8;

    const int stride = options.size.width() * bytesPerPixel;

    // the buffer size needs to be aligned to the next page
    const uint64_t bufferSize = std::ceil(options.size.height() * stride / double(getpagesize())) * getpagesize();

#if HAVE_MEMFD
    FileDescriptor fd = FileDescriptor(memfd_create("shm", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd.isValid()) {
        return nullptr;
    }

    if (ftruncate(fd.get(), bufferSize) < 0) {
        return nullptr;
    }

    fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
#else
    char templateName[] = "/tmp/kwin-shm-XXXXXX";
    FileDescriptor fd{mkstemp(templateName)};
    if (!fd.isValid()) {
        return nullptr;
    }

    unlink(templateName);
    int flags = fcntl(fd.get(), F_GETFD);
    if (flags == -1 || fcntl(fd.get(), F_SETFD, flags | FD_CLOEXEC) == -1) {
        return nullptr;
    }

    if (ftruncate(fd.get(), bufferSize) < 0) {
        return nullptr;
    }
#endif

    struct udmabuf_create create{
        .memfd = uint32_t(fd.get()),
        .flags = 0,
        .offset = 0,
        .size = bufferSize,
    };
    int err = ioctl(dev.get(), UDMABUF_CREATE, &create);
    if (err != 0) {
        qCWarning(KWIN_CORE, "Could not create udmabuf: %s", strerror(errno));
        return nullptr;
    }

    DmaBufAttributes attributes;
    attributes.planeCount = 1;
    attributes.width = options.size.width();
    attributes.height = options.size.height();
    attributes.format = options.format;
    attributes.modifier = DRM_FORMAT_MOD_LINEAR;
    attributes.device = devNum;
    attributes.fd[0] = std::move(fd);
    attributes.offset[0] = 0;
    attributes.pitch[0] = stride;

    MemoryMap memoryMap(attributes.pitch[0] * attributes.height, PROT_READ | PROT_WRITE, MAP_SHARED, attributes.fd[0].get(), attributes.offset[0]);
    if (!memoryMap.isValid()) {
        return nullptr;
    }

    return new UDmabufGraphicsBuffer(std::move(attributes), std::move(memoryMap));
#else
    Q_UNREACHABLE();
    return nullptr;
#endif
}

GbmGraphicsBufferAllocator::GbmGraphicsBufferAllocator(gbm_device *device, dev_t deviceId)
    : m_gbmDevice(device)
    , m_deviceId(deviceId)
#if defined(Q_OS_LINUX)
    , m_udmabuf(::open("/dev/udmabuf", O_RDWR | O_CLOEXEC))
#else
    , m_udmabuf(-1)
#endif
{
}

GbmGraphicsBufferAllocator::~GbmGraphicsBufferAllocator()
{
}

static GraphicsBuffer *allocateDumb(gbm_device *device, dev_t deviceId, const GraphicsBufferOptions &options)
{
    if (!options.modifiers.empty()) {
        return nullptr;
    }

    drm_mode_create_dumb createArgs{
        .height = uint32_t(options.size.height()),
        .width = uint32_t(options.size.width()),
        .bpp = 32,
    };
    if (drmIoctl(gbm_device_get_fd(device), DRM_IOCTL_MODE_CREATE_DUMB, &createArgs) != 0) {
        qCWarning(KWIN_CORE) << "DRM_IOCTL_MODE_CREATE_DUMB failed:" << strerror(errno);
        return nullptr;
    }

    int primeFd;
    if (drmPrimeHandleToFD(gbm_device_get_fd(device), createArgs.handle, DRM_CLOEXEC, &primeFd) != 0) {
        qCWarning(KWIN_CORE) << "drmPrimeHandleToFD() failed:" << strerror(errno);
        drm_mode_destroy_dumb destroyArgs{
            .handle = createArgs.handle,
        };
        drmIoctl(gbm_device_get_fd(device), DRM_IOCTL_MODE_DESTROY_DUMB, &destroyArgs);
        return nullptr;
    }

    return new DumbGraphicsBuffer(gbm_device_get_fd(device), createArgs.handle, DmaBufAttributes{
                                                                                    .planeCount = 1,
                                                                                    .width = options.size.width(),
                                                                                    .height = options.size.height(),
                                                                                    .format = options.format,
                                                                                    .modifier = DRM_FORMAT_MOD_LINEAR,
                                                                                    .device = deviceId,
                                                                                    .fd = {FileDescriptor(primeFd), FileDescriptor{}, FileDescriptor{}, FileDescriptor{}},
                                                                                    .offset = {0, 0, 0, 0},
                                                                                    .pitch = {createArgs.pitch, 0, 0, 0},
                                                                                });
}

static GraphicsBuffer *allocateDmaBuf(gbm_device *device, dev_t deviceId, const GraphicsBufferOptions &options)
{
    uint32_t flags = GBM_BO_USE_RENDERING;
    if (options.scanout) {
        flags |= GBM_BO_USE_SCANOUT;
    }
    if (!options.modifiers.empty() && !(options.modifiers.size() == 1 && options.modifiers.front() == DRM_FORMAT_MOD_INVALID)) {
        gbm_bo *bo = gbm_bo_create_with_modifiers2(device,
                                                   options.size.width(),
                                                   options.size.height(),
                                                   options.format,
                                                   options.modifiers.data(),
                                                   options.modifiers.size(),
                                                   flags);
        if (bo) {
            std::optional<DmaBufAttributes> attributes = dmaBufAttributesForBo(bo, deviceId);
            if (!attributes.has_value()) {
                gbm_bo_destroy(bo);
                return nullptr;
            }
            return new GbmGraphicsBuffer(std::move(attributes.value()), bo);
        }
    }

    if (options.modifiers.size() == 1 && options.modifiers.front() == DRM_FORMAT_MOD_LINEAR) {
        flags |= GBM_BO_USE_LINEAR;
    } else if (!options.modifiers.empty() && !options.modifiers.contains(DRM_FORMAT_MOD_INVALID)) {
        return nullptr;
    }

    gbm_bo *bo = gbm_bo_create(device,
                               options.size.width(),
                               options.size.height(),
                               options.format,
                               flags);
    if (bo) {
        std::optional<DmaBufAttributes> attributes = dmaBufAttributesForBo(bo, deviceId);
        if (!attributes.has_value()) {
            gbm_bo_destroy(bo);
            return nullptr;
        }
        if (flags & GBM_BO_USE_LINEAR) {
            attributes->modifier = DRM_FORMAT_MOD_LINEAR;
        } else {
            attributes->modifier = DRM_FORMAT_MOD_INVALID;
        }
        return new GbmGraphicsBuffer(std::move(attributes.value()), bo);
    }

    return nullptr;
}

GraphicsBuffer *GbmGraphicsBufferAllocator::allocate(const GraphicsBufferOptions &options)
{
    if (options.software) {
        if (!options.scanout && m_udmabuf.isValid()) {
            GraphicsBuffer *ret = allocateUdmabuf(m_udmabuf, m_deviceId, options);
            if (ret) {
                return ret;
            }
        }
        return allocateDumb(m_gbmDevice, m_deviceId, options);
    }

    return allocateDmaBuf(m_gbmDevice, m_deviceId, options);
}

GbmGraphicsBuffer::GbmGraphicsBuffer(DmaBufAttributes attributes, gbm_bo *handle)
    : m_bo(handle)
    , m_dmabufAttributes(std::move(attributes))
    , m_size(m_dmabufAttributes.width, m_dmabufAttributes.height)
    , m_hasAlphaChannel(alphaChannelFromDrmFormat(m_dmabufAttributes.format))
{
}

GbmGraphicsBuffer::~GbmGraphicsBuffer()
{
    unmap();
    gbm_bo_destroy(m_bo);
}

QSize GbmGraphicsBuffer::size() const
{
    return m_size;
}

bool GbmGraphicsBuffer::hasAlphaChannel() const
{
    return m_hasAlphaChannel;
}

const DmaBufAttributes *GbmGraphicsBuffer::dmabufAttributes() const
{
    return &m_dmabufAttributes;
}

GraphicsBuffer::Map GbmGraphicsBuffer::map(MapFlags flags)
{
    if (!m_mapPtr) {
        uint32_t access = 0;
        if (flags & MapFlag::Read) {
            access |= GBM_BO_TRANSFER_READ;
        }
        if (flags & MapFlag::Write) {
            access |= GBM_BO_TRANSFER_WRITE;
        }
        m_mapPtr = gbm_bo_map(m_bo, 0, 0, m_dmabufAttributes.width, m_dmabufAttributes.height, access, &m_mapStride, &m_mapData);
    }
    return Map{
        .data = m_mapPtr,
        .stride = m_mapStride,
    };
}

void GbmGraphicsBuffer::unmap()
{
    if (m_mapPtr) {
        gbm_bo_unmap(m_bo, m_mapData);
        m_mapPtr = nullptr;
        m_mapData = nullptr;
    }
}

DumbGraphicsBuffer::DumbGraphicsBuffer(int drmFd, uint32_t handle, DmaBufAttributes attributes)
    : m_drmFd(drmFd)
    , m_handle(handle)
    , m_size(attributes.pitch[0] * attributes.height)
    , m_dmabufAttributes(std::move(attributes))
    , m_hasAlphaChannel(alphaChannelFromDrmFormat(m_dmabufAttributes.format))
{
}

DumbGraphicsBuffer::~DumbGraphicsBuffer()
{
    unmap();

    drm_mode_destroy_dumb destroyArgs{
        .handle = m_handle,
    };
    drmIoctl(m_drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyArgs);
}

QSize DumbGraphicsBuffer::size() const
{
    return QSize(m_dmabufAttributes.width, m_dmabufAttributes.height);
}

bool DumbGraphicsBuffer::hasAlphaChannel() const
{
    return m_hasAlphaChannel;
}

const DmaBufAttributes *DumbGraphicsBuffer::dmabufAttributes() const
{
    return &m_dmabufAttributes;
}

GraphicsBuffer::Map DumbGraphicsBuffer::map(MapFlags flags)
{
    if (!m_data) {
        drm_mode_map_dumb mapArgs{
            .handle = m_handle,
        };
        if (drmIoctl(m_drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mapArgs) != 0) {
            qCWarning(KWIN_CORE) << "DRM_IOCTL_MODE_MAP_DUMB failed:" << strerror(errno);
            return {};
        }

        void *address = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_drmFd, mapArgs.offset);
        if (address == MAP_FAILED) {
            qCWarning(KWIN_CORE) << "mmap() failed:" << strerror(errno);
            return {};
        }

        m_data = address;
    }
    return Map{
        .data = m_data,
        .stride = m_dmabufAttributes.pitch[0],
    };
}

void DumbGraphicsBuffer::unmap()
{
    if (m_data) {
        munmap(m_data, m_size);
        m_data = nullptr;
    }
}

} // namespace KWin

#include "gbmgraphicsbufferallocator.moc"
#include "moc_gbmgraphicsbufferallocator.cpp"
