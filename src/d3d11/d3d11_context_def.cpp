#include "d3d11_context_def.h"
#include "d3d11_device.h"

namespace dxvk {
  
  D3D11DeferredContext::D3D11DeferredContext(
          D3D11Device*    pParent,
    const Rc<DxvkDevice>& Device,
          UINT            ContextFlags)
  : D3D11DeviceContext(pParent, Device, GetCsChunkFlags(pParent)),
    m_contextFlags(ContextFlags),
    m_commandList (CreateCommandList()) {
    ClearState();
  }
  
  
  D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE D3D11DeferredContext::GetType() {
    return D3D11_DEVICE_CONTEXT_DEFERRED;
  }
  
  
  UINT STDMETHODCALLTYPE D3D11DeferredContext::GetContextFlags() {
    return m_contextFlags;
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::GetData(
          ID3D11Asynchronous*               pAsync,
          void*                             pData,
          UINT                              DataSize,
          UINT                              GetDataFlags) {
    Logger::err("D3D11: GetData called on a deferred context");
    return DXGI_ERROR_INVALID_CALL;
  }


  void STDMETHODCALLTYPE D3D11DeferredContext::Flush() {
    Logger::err("D3D11: Flush called on a deferred context");
  }
  
  
  void STDMETHODCALLTYPE D3D11DeferredContext::ExecuteCommandList(
          ID3D11CommandList*  pCommandList,
          BOOL                RestoreContextState) {
    D3D10DeviceLock lock = LockContext();

    FlushCsChunk();
    
    static_cast<D3D11CommandList*>(pCommandList)->EmitToCommandList(m_commandList.ptr());
    
    if (RestoreContextState)
      RestoreState();
    else
      ClearState();
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::FinishCommandList(
          BOOL                RestoreDeferredContextState,
          ID3D11CommandList   **ppCommandList) {
    D3D10DeviceLock lock = LockContext();

    FlushCsChunk();
    
    if (ppCommandList != nullptr)
      *ppCommandList = m_commandList.ref();
    m_commandList = CreateCommandList();
    
    if (RestoreDeferredContextState)
      RestoreState();
    else
      ClearState();
    
    m_mappedResources.clear();
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::Map(
          ID3D11Resource*             pResource,
          UINT                        Subresource,
          D3D11_MAP                   MapType,
          UINT                        MapFlags,
          D3D11_MAPPED_SUBRESOURCE*   pMappedResource) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(!pResource || !pMappedResource))
      return E_INVALIDARG;
    
    D3D11_RESOURCE_DIMENSION resourceDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    pResource->GetType(&resourceDim);
    
    if (MapType == D3D11_MAP_WRITE_DISCARD) {
      D3D11DeferredContextMapEntry entry;
      
      HRESULT status = resourceDim == D3D11_RESOURCE_DIMENSION_BUFFER
        ? MapBuffer(pResource,              MapType, MapFlags, &entry)
        : MapImage (pResource, Subresource, MapType, MapFlags, &entry);
      
      if (unlikely(FAILED(status))) {
        *pMappedResource = D3D11_MAPPED_SUBRESOURCE();
        return status;
      }
      
      // Adding a new map entry actually overrides the
      // old one in practice because the lookup function
      // scans the array in reverse order
      m_mappedResources.push_back(entry);
      
      // Fill mapped resource structure
      pMappedResource->pData      = entry.MapPointer;
      pMappedResource->RowPitch   = entry.RowPitch;
      pMappedResource->DepthPitch = entry.DepthPitch;
      return S_OK;
    } else if (MapType == D3D11_MAP_WRITE_NO_OVERWRITE) {
      // The resource must be mapped with D3D11_MAP_WRITE_DISCARD
      // before it can be mapped with D3D11_MAP_WRITE_NO_OVERWRITE.
      auto entry = FindMapEntry(pResource, Subresource);
      
      if (unlikely(entry == m_mappedResources.rend())) {
        *pMappedResource = D3D11_MAPPED_SUBRESOURCE();
        return E_INVALIDARG;
      }
      
      // Return same memory region as earlier
      entry->MapType = D3D11_MAP_WRITE_NO_OVERWRITE;
      
      pMappedResource->pData      = entry->MapPointer;
      pMappedResource->RowPitch   = entry->RowPitch;
      pMappedResource->DepthPitch = entry->DepthPitch;
      return S_OK;
    } else {
      // Not allowed on deferred contexts
      *pMappedResource = D3D11_MAPPED_SUBRESOURCE();
      return E_INVALIDARG;
    }
  }
  
  
  void STDMETHODCALLTYPE D3D11DeferredContext::Unmap(
          ID3D11Resource*             pResource,
          UINT                        Subresource) {
    // No-op, updates are committed in Map
  }
  
  
  void STDMETHODCALLTYPE D3D11DeferredContext::SwapDeviceContextState(
          ID3DDeviceContextState*           pState,
          ID3DDeviceContextState**          ppPreviousState) {
    Logger::err("D3D11: SwapDeviceContextState called on a deferred context");
  }


  HRESULT D3D11DeferredContext::MapBuffer(
          ID3D11Resource*               pResource,
          D3D11_MAP                     MapType,
          UINT                          MapFlags,
          D3D11DeferredContextMapEntry* pMapEntry) {
    D3D11Buffer* pBuffer = static_cast<D3D11Buffer*>(pResource);
    
    if (unlikely(pBuffer->GetMapMode() == D3D11_COMMON_BUFFER_MAP_MODE_NONE)) {
      Logger::err("D3D11: Cannot map a device-local buffer");
      return E_INVALIDARG;
    }
    
    pMapEntry->pResource    = pResource;
    pMapEntry->Subresource  = 0;
    pMapEntry->MapType      = D3D11_MAP_WRITE_DISCARD;
    pMapEntry->RowPitch     = pBuffer->Desc()->ByteWidth;
    pMapEntry->DepthPitch   = pBuffer->Desc()->ByteWidth;
    
    if (likely(pBuffer->Desc()->Usage == D3D11_USAGE_DYNAMIC && m_csFlags.test(DxvkCsChunkFlag::SingleUse))) {
      // For resources that cannot be written by the GPU,
      // we may write to the buffer resource directly and
      // just swap in the buffer slice as needed.
      pMapEntry->BufferSlice = pBuffer->AllocSlice();
      pMapEntry->MapPointer  = pMapEntry->BufferSlice.mapPtr;

      EmitCs([
        cDstBuffer = pBuffer->GetBuffer(),
        cPhysSlice = pMapEntry->BufferSlice
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cDstBuffer, cPhysSlice);
      });
    } else {
      // For GPU-writable resources, we need a data slice
      // to perform the update operation at execution time.
      pMapEntry->DataSlice   = AllocUpdateBufferSlice(pBuffer->Desc()->ByteWidth);
      pMapEntry->MapPointer  = pMapEntry->DataSlice.ptr();

      EmitCs([
        cDstBuffer = pBuffer->GetBuffer(),
        cDataSlice = pMapEntry->DataSlice
      ] (DxvkContext* ctx) {
        DxvkBufferSliceHandle slice = cDstBuffer->allocSlice();
        std::memcpy(slice.mapPtr, cDataSlice.ptr(), cDataSlice.length());
        ctx->invalidateBuffer(cDstBuffer, slice);
      });
    }
    
    return S_OK;
  }
  
  
  HRESULT D3D11DeferredContext::MapImage(
          ID3D11Resource*               pResource,
          UINT                          Subresource,
          D3D11_MAP                     MapType,
          UINT                          MapFlags,
          D3D11DeferredContextMapEntry* pMapEntry) {
    const D3D11CommonTexture* pTexture = GetCommonTexture(pResource);
    const Rc<DxvkImage> image = pTexture->GetImage();
    
    if (unlikely(pTexture->GetMapMode() == D3D11_COMMON_TEXTURE_MAP_MODE_NONE)) {
      Logger::err("D3D11: Cannot map a device-local image");
      return E_INVALIDARG;
    }

    if (unlikely(Subresource >= pTexture->CountSubresources()))
      return E_INVALIDARG;
    
    VkFormat packedFormat = m_parent->LookupPackedFormat(
      pTexture->Desc()->Format, pTexture->GetFormatMode()).Format;
    
    auto formatInfo = imageFormatInfo(packedFormat);
    auto subresource = pTexture->GetSubresourceFromIndex(
        formatInfo->aspectMask, Subresource);
    
    VkExtent3D levelExtent = image->mipLevelExtent(subresource.mipLevel);
    VkExtent3D blockCount = util::computeBlockCount(
      levelExtent, formatInfo->blockSize);
    
    VkDeviceSize eSize = formatInfo->elementSize;
    VkDeviceSize xSize = blockCount.width  * eSize;
    VkDeviceSize ySize = blockCount.height * xSize;
    VkDeviceSize zSize = blockCount.depth  * ySize;
    
    pMapEntry->pResource    = pResource;
    pMapEntry->Subresource  = Subresource;
    pMapEntry->MapType      = D3D11_MAP_WRITE_DISCARD;
    pMapEntry->RowPitch     = xSize;
    pMapEntry->DepthPitch   = ySize;
    pMapEntry->DataSlice    = AllocUpdateBufferSlice(zSize);
    pMapEntry->MapPointer   = pMapEntry->DataSlice.ptr();

    EmitCs([
      cImage              = pTexture->GetImage(),
      cSubresource        = pTexture->GetSubresourceFromIndex(
        VK_IMAGE_ASPECT_COLOR_BIT, Subresource),
      cDataSlice          = pMapEntry->DataSlice,
      cDataPitchPerRow    = pMapEntry->RowPitch,
      cDataPitchPerLayer  = pMapEntry->DepthPitch,
      cPackedFormat       = GetPackedDepthStencilFormat(pTexture->Desc()->Format)
    ] (DxvkContext* ctx) {
      VkImageSubresourceLayers srLayers;
      srLayers.aspectMask     = cSubresource.aspectMask;
      srLayers.mipLevel       = cSubresource.mipLevel;
      srLayers.baseArrayLayer = cSubresource.arrayLayer;
      srLayers.layerCount     = 1;

      VkOffset3D mipLevelOffset = { 0, 0, 0 };
      VkExtent3D mipLevelExtent = cImage->mipLevelExtent(srLayers.mipLevel);
      
      if (cPackedFormat == VK_FORMAT_UNDEFINED) {
        ctx->updateImage(
          cImage, srLayers,
          mipLevelOffset,
          mipLevelExtent,
          cDataSlice.ptr(),
          cDataPitchPerRow,
          cDataPitchPerLayer);
      } else {
        ctx->updateDepthStencilImage(
          cImage, srLayers,
          VkOffset2D { mipLevelOffset.x,     mipLevelOffset.y      },
          VkExtent2D { mipLevelExtent.width, mipLevelExtent.height },
          cDataSlice.ptr(),
          cDataPitchPerRow,
          cDataPitchPerLayer,
          cPackedFormat);
      }
    });

    return S_OK;
  }
  
  
  Com<D3D11CommandList> D3D11DeferredContext::CreateCommandList() {
    return new D3D11CommandList(m_parent, m_contextFlags);
  }
  
  
  void D3D11DeferredContext::EmitCsChunk(DxvkCsChunkRef&& chunk) {
    m_commandList->AddChunk(std::move(chunk));
  }


  DxvkCsChunkFlags D3D11DeferredContext::GetCsChunkFlags(
          D3D11Device*                  pDevice) {
    return pDevice->GetOptions()->dcSingleUseMode
      ? DxvkCsChunkFlags(DxvkCsChunkFlag::SingleUse)
      : DxvkCsChunkFlags();
  }

}