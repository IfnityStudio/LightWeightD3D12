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

#ifndef LIGHTD3D12_SINGLE_DIRECT_QUEUE
#define LIGHTD3D12_SINGLE_DIRECT_QUEUE 1
#endif

namespace lightd3d12::detail
{
	inline HRESULT CheckResult( HRESULT hr, const char* expression, const char* message )
	{
		if( FAILED( hr ) )
		{
			std::string error = message != nullptr && message[ 0 ] != '\0' ? std::string( message ) : std::string( "HRESULT call failed." );
			if( expression != nullptr && expression[ 0 ] != '\0' )
			{
				error += " [";
				error += expression;
				error += "]";
			}

			throw std::runtime_error( error );
		}

		return hr;
	}

	inline void ThrowIfFailed( HRESULT hr, const char* message )
	{
		static_cast<void>( CheckResult( hr, nullptr, message ) );
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

#define C_RESULT( expression, message ) ::lightd3d12::detail::CheckResult( ( expression ), #expression, ( message ) )
