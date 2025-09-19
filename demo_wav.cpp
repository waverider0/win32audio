#include "win32audio.h"
#include "typedefs.h"

#define PATH_TO_WAV "w:/win32audio/sound.wav"

struct WAV
{
  u8 *samples;
  u32 samples_size;
  WAVEFORMATEX *format;
};

// NOTE: for packing more than one variable in `LPVOID thread_data`
struct PlayWavThreadData
{
  Win32Audio *wa;
  WAV wav;
};

WAV load_wav(char *path)
{
  WAV wav = {};

  // read file into memory

  HANDLE file_handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  assert(file_handle != INVALID_HANDLE_VALUE);

  LARGE_INTEGER lpFileSizeHigh;
  assert(GetFileSizeEx(file_handle, &lpFileSizeHigh));

  DWORD file_size = (DWORD)lpFileSizeHigh.QuadPart;
  assert(file_size < 1 << 20); // max 1 MB

  void *file_data = VirtualAlloc(NULL, file_size, MEM_COMMIT, PAGE_READWRITE);
  assert(file_data);

  DWORD n_bytes_read;
  assert(ReadFile(file_handle, file_data, file_size, &n_bytes_read, NULL) || n_bytes_read == file_size);

  CloseHandle(file_handle);
  u8 *ptr = (u8 *)file_data;

  // parse wav header

  assert(memcmp(ptr, "RIFF", 4) == 0);
  ptr += 8;

  assert(memcmp(ptr, "WAVE", 4) == 0);
  ptr += 4;

  // all RIFF headers follow the general pattern of: chunk id, chunk size, chunk data, repeat.
  // 'fmt' and 'data' values can be at arbitrary positions so we use a loop to ensure they get correctly parsed
  while (ptr < (u8 *)file_data + file_size)
  {
    u8 *chunk_id = ptr;
    ptr += 4;
    u32 chunk_size = *(u32 *)ptr;
    ptr += 4;

    if (memcmp(chunk_id, "fmt ", 4) == 0)
    {
      wav.format = (WAVEFORMATEX *)ptr;
    }
    else if (memcmp(chunk_id, "data", 4) == 0)
    {
      wav.samples = ptr;
      wav.samples_size = chunk_size;
    }

    ptr += chunk_size;
    if (chunk_size % 2 != 0)
      ptr++; // pad to even byte boundary
  }

  return wav;
}

DWORD WINAPI play_wav(LPVOID thread_data)
{
  PlayWavThreadData *data = (PlayWavThreadData *)thread_data;
  Win32Audio *wa = data->wa;
  WAV wav = data->wav;

  u32 bytes_per_frame = wav.format->nChannels * (wav.format->wBitsPerSample / 8);
  u32 total_frames = wav.samples_size / bytes_per_frame;

  f32 fractional_position = 0.0;

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

    f32 resample_ratio = (f32)wav.format->nSamplesPerSec / wa->device_waveformat->nSamplesPerSec;

    for (u32 i = 0; i < n_frames_to_buffer; i++)
    {
      u32 index = (u32)fractional_position;
      f32 frac = fractional_position - index;

      if (index >= total_frames)
        break;

      // resample for pitch correction
      assert(wav.format->wBitsPerSample == 16);
      i16 *wav_samples = (i16 *)wav.samples;
      f32 sample0 = wav_samples[index * wav.format->nChannels] / 32768.0f;
      f32 sample1 = wav_samples[(index + 1) * wav.format->nChannels] / 32768.0f;
      f32 resampled = sample0 + (sample1 - sample0) * frac; // lerp

      // duplicate mono across all channels
      for (u32 c = 0; c < wa->device_waveformat->nChannels; c++)
        *os_ringbuffer++ = resampled;

      fractional_position += resample_ratio;
    }

    // submit buffer to audio engine and mark for reuse
    WA_run_with_device_fallback(wa, [&]() -> HRESULT { return wa->AudioRenderClient->ReleaseBuffer(n_frames_to_buffer, 0); });

    if (fractional_position >= total_frames)
      break;
  }

  return 0;
}

i32 main()
{
  Win32Audio wa = WA_create();
  WAV wav = load_wav(PATH_TO_WAV);

  PlayWavThreadData thread_data = { &wa, wav };
  HANDLE wav_thread = CreateThread(NULL, 0, play_wav, &thread_data, 0, NULL);

  // do something on the main thread
  for (i32 i = 0; i < 3; i++)
  {
    Sleep(1000);
    printf("%d\n", i);
  }

  WaitForSingleObject(wav_thread, INFINITE);
  CloseHandle(wav_thread);

  VirtualFree(wav.samples, wav.samples_size, MEM_RELEASE);
  WA_destroy(&wa);
  return 0;
}