#pragma once

#include "LightD3D12/LightD3D12.hpp"
#include "../../D3D12/d3dx12.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include <d3dcompiler.h>

#if defined( _DEBUG )
#include <d3d12sdklayers.h>
#include <dxgidebug.h>
#endif

namespace lightd3d12::detail
{
	inline void ThrowIfFailed( HRESULT hr, const char* message )
	{
		if( FAILED( hr ) )
		{
			throw std::runtime_error( message );
		}
	}

	inline std::wstring ToWide( const std::string& value )
	{
		return std::wstring( value.begin(), value.end() );
	}

	inline void SetDebugName( ID3D12Object* object, const std::string& name )
	{
		if( object == nullptr || name.empty() )
		{
			return;
		}

		const std::wstring wide = ToWide( name );
		object->SetName( wide.c_str() );
	}

	inline HWND GetHwnd( const NativeWindowHandle& handle )
	{
		if( !handle.Valid() || handle.type != NativeWindowHandle::Type::Win32Hwnd )
		{
			throw std::runtime_error( "LightD3D12 necesita un HWND valido para crear la swapchain." );
		}

		return static_cast< HWND >(handle.value);
	}

	inline UINT Align256( UINT value )
	{
		return (value + 255u) & ~255u;
	}
}
