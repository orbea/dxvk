#pragma once

#include "../dxvk/dxvk_device.h"

#include "../d3d10/d3d10_view_srv.h"

#include "d3d11_device_child.h"
#include "d3d11_view.h"

namespace dxvk {
  
  class D3D11Device;
  
  /**
   * \brief Shader resource view
   */
  class D3D11ShaderResourceView : public D3D11DeviceChild<ID3D11ShaderResourceView> {
    
  public:
    
    D3D11ShaderResourceView(
            D3D11Device*                      pDevice,
            ID3D11Resource*                   pResource,
      const D3D11_SHADER_RESOURCE_VIEW_DESC*  pDesc);
    
    ~D3D11ShaderResourceView();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) final;
    
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) final;
    
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** ppResource) final;
    
    void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc) final;
    
    const D3D11_VK_VIEW_INFO& GetViewInfo() const {
      return m_info;
    }

    BOOL TestHazards() const {
      return m_info.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS);
    }

    D3D11_RESOURCE_DIMENSION GetResourceType() const {
      D3D11_RESOURCE_DIMENSION type;
      m_resource->GetType(&type);
      return type;
    }

    D3D11_COMMON_RESOURCE_DESC GetResourceDesc() const {
      D3D11_COMMON_RESOURCE_DESC desc;
      GetCommonResourceDesc(m_resource, &desc);
      return desc;
    }
    
    Rc<DxvkBufferView> GetBufferView() const {
      return m_bufferView;
    }
    
    Rc<DxvkImageView> GetImageView() const {
      return m_imageView;
    }

    D3D10ShaderResourceView* GetD3D10Iface() {
      return &m_d3d10;
    }
    
    static HRESULT GetDescFromResource(
            ID3D11Resource*                   pResource,
            D3D11_SHADER_RESOURCE_VIEW_DESC*  pDesc);
    
    static HRESULT NormalizeDesc(
            ID3D11Resource*                   pResource,
            D3D11_SHADER_RESOURCE_VIEW_DESC*  pDesc);
    
  private:
    
    Com<D3D11Device>                  m_device;
    ID3D11Resource*                   m_resource;
    D3D11_SHADER_RESOURCE_VIEW_DESC   m_desc;
    D3D11_VK_VIEW_INFO                m_info;
    Rc<DxvkBufferView>                m_bufferView;
    Rc<DxvkImageView>                 m_imageView;
    D3D10ShaderResourceView           m_d3d10;

  };
  
}
