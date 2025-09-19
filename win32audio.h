#pragma once
#pragma comment(lib, "ole32")

#include "typedefs.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <assert.h>

#define HR(statement) do { HRESULT _hr = statement; assert(SUCCEEDED(_hr)); } while (0)

class NotificationClient : public IMMNotificationClient {
public:
  LONG ref_count = 1;
  volatile BOOL device_changed = FALSE;

  ULONG STDMETHODCALLTYPE AddRef()
  {
    return InterlockedIncrement(&ref_count);
  }

  ULONG STDMETHODCALLTYPE Release()
  {
    ULONG ref = InterlockedDecrement(&ref_count);
    if (ref == 0)
      delete this;
    return ref;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppv)
  {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient))
    {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state)
  {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR device_id)
  {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR device_id)
  {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR device_id)
  {
    if (flow == eRender && role == eConsole)
      device_changed = TRUE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR device_id, const PROPERTYKEY key)
  {
    return S_OK;
  }
};

struct Win32Audio
{
  // COM
  IMMDeviceEnumerator *MMDeviceEnumerator;
  IMMDevice *MMDevice;
  NotificationClient *notification_client;
  IAudioClient3 *AudioClient3;
  IAudioRenderClient *AudioRenderClient;
  IAudioClock *AudioClock;

  // playback
  WAVEFORMATEX *device_waveformat;
  u32 os_buffer_size_in_frames;
  HANDLE os_buffer_ready_event;
};

void WA_start(Win32Audio *wa)
{
  HR(wa->MMDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &wa->MMDevice));
  HR(wa->MMDevice->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void **)&wa->AudioClient3));
  REFERENCE_TIME default_period, minimum_period;
  HR(wa->AudioClient3->GetDevicePeriod(&default_period, &minimum_period));
  HR(wa->AudioClient3->GetMixFormat(&wa->device_waveformat));
  HR(wa->AudioClient3->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum_period, 0, wa->device_waveformat, NULL));
  HR(wa->AudioClient3->GetService(__uuidof(IAudioRenderClient), (void **)&wa->AudioRenderClient));
  HR(wa->AudioClient3->GetService(__uuidof(IAudioClock), (void **)&wa->AudioClock));
  HR(wa->AudioClient3->GetBufferSize(&wa->os_buffer_size_in_frames));
  wa->os_buffer_ready_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  HR(wa->AudioClient3->SetEventHandle(wa->os_buffer_ready_event));
  HR(wa->AudioClient3->Start());
}

void WA_stop(Win32Audio *wa)
{
  if (wa->AudioClient3)
  {
    HR(wa->AudioClient3->Stop());
    wa->AudioClient3->Release();
    wa->AudioClient3 = NULL;
  }
  if (wa->AudioRenderClient)
  {
    wa->AudioRenderClient->Release();
    wa->AudioRenderClient = NULL;
  }
  if (wa->AudioClock)
  {
    wa->AudioClock->Release();
    wa->AudioClock = NULL;
  }
  if (wa->MMDevice)
  {
    wa->MMDevice->Release();
    wa->MMDevice = NULL;
  }
  if (wa->device_waveformat)
  {
    CoTaskMemFree(wa->device_waveformat);
    wa->device_waveformat = NULL;
  }
  if (wa->os_buffer_ready_event)
  {
    CloseHandle(wa->os_buffer_ready_event);
    wa->os_buffer_ready_event = NULL;
  }
  wa->os_buffer_size_in_frames = 0;
  wa->notification_client->device_changed = FALSE;
}

Win32Audio WA_create()
{
  Win32Audio wa = {};

  HR(CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE));
  HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (LPVOID *)&wa.MMDeviceEnumerator));
  wa.notification_client = new NotificationClient();
  HR(wa.MMDeviceEnumerator->RegisterEndpointNotificationCallback(wa.notification_client));
  WA_start(&wa);

  return wa;
}

void WA_destroy(Win32Audio *wa)
{
  WA_stop(wa);
  if (wa->MMDeviceEnumerator)
  {
    if (wa->notification_client)
      HR(wa->MMDeviceEnumerator->UnregisterEndpointNotificationCallback(wa->notification_client));
    wa->MMDeviceEnumerator->Release();
    wa->MMDeviceEnumerator = NULL;
  }
  if (wa->notification_client)
  {
    wa->notification_client->Release();
    wa->notification_client = NULL;
  }
}

// device invalidated (e.g: unplugging headphones)
#include <functional>
void WA_run_with_device_fallback(Win32Audio *wa, std::function<HRESULT()> f)
{
  HRESULT hr = f();
  if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
  {
    WA_stop(wa);
    Sleep(200);
    WA_start(wa);
    hr = f();
  }
  assert(SUCCEEDED(hr));
}
