#include "win32audio.h"
#include "typedefs.h"

#include <math.h> // for sine
#include <stdio.h> // for printf

#define PI 3.14159
#define SINE_FREQUENCY 440

DWORD WINAPI play_sine(LPVOID thread_data)
{
  Win32Audio *wa = (Win32Audio *)thread_data;

  f64 time = 0.0;

  for (;;)
  {
    // block until space is avaiable in the os buffer. break if wait failed (e.g., audio client stopped)
    if (WaitForSingleObject(wa->os_buffer_ready_event, INFINITE) != WAIT_OBJECT_0)
      break;

    // default device changes (e.g: plugging in headphones)
    if (wa->notification_client->device_changed)
    {
      WA_stop(wa);
      Sleep(200);
      WA_start(wa);
    }

    u32 n_frames_buffered;
    WA_run_with_device_fallback(wa, [&]() -> HRESULT { return wa->AudioClient3->GetCurrentPadding(&n_frames_buffered); });
    u32 n_frames_to_buffer = wa->os_buffer_size_in_frames - n_frames_buffered;;
    f32 *os_ringbuffer;
    WA_run_with_device_fallback(wa, [&]() -> HRESULT { return wa->AudioRenderClient->GetBuffer(n_frames_to_buffer, (BYTE **)&os_ringbuffer); });

    // derivation:
    // sin(2π * Frequency * Time) = sin(SampleIndex * TimeStep)
    // Time = SampleIndex / SampleRate
    // TimeStep = 2π * Frequency / SampleRate
    f64 step = 2.0 * PI * SINE_FREQUENCY / wa->device_waveformat->nSamplesPerSec;

    for (u32 i = 0; i < n_frames_to_buffer; i++)
    {
      f32 amplitude = (f32)(sin(time));
      for (u32 c = 0; c < wa->device_waveformat->nChannels; c++)
        *os_ringbuffer++ = amplitude;

      time += step;
    }

    // submit buffer to audio engine and mark for reuse
    WA_run_with_device_fallback(wa, [&]() -> HRESULT { return wa->AudioRenderClient->ReleaseBuffer(n_frames_to_buffer, 0); });
  }

  return 0;
}

i32 main()
{
  Win32Audio wa = WA_create();

  HANDLE sine_thread = CreateThread(NULL, 0, play_sine, &wa, 0, NULL);

  // do something on the main thread
  for (i32 i = 0 ;; i++)
  {
    Sleep(1000);
    printf("%d\n", i);
  }

  WaitForSingleObject(sine_thread, INFINITE);
  CloseHandle(sine_thread);

  WA_destroy(&wa);
  return 0;
}