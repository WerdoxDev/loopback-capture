//! Event-driven WASAPI capture. All audio interfaces live on one MTA worker.
//!
//! Process-loopback activation follows Microsoft's ApplicationLoopback sample
//! (Windows-classic-samples, MIT). Unlike that sample, capture does not use MF
//! work queues: Rust ownership orders packet, client, event, and COM teardown.

use std::{
    mem::{size_of, ManuallyDrop},
    ptr,
    sync::mpsc::{self, Receiver, SyncSender, TryRecvError},
    time::{Duration, Instant},
};
use windows::{
    core::{implement, w, Interface, Ref, HRESULT},
    Win32::{
        Foundation::{CloseHandle, HANDLE, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT},
        Media::Audio::*,
        System::{
            Com::{
                CoCreateInstance, CoInitializeEx, CoUninitialize,
                StructuredStorage::{
                    PROPVARIANT, PROPVARIANT_0, PROPVARIANT_0_0, PROPVARIANT_0_0_0,
                },
                BLOB, CLSCTX_ALL, COINIT_MULTITHREADED,
            },
            Threading::{
                AvRevertMmThreadCharacteristics, AvSetMmThreadCharacteristicsW, CreateEventW,
                WaitForSingleObject,
            },
            Variant::VT_BLOB,
        },
    },
};

use crate::capture::{AudioSink, CaptureMode};

const STOP_POLL: Duration = Duration::from_millis(25);
const ACTIVATION_TIMEOUT: Duration = Duration::from_secs(5);
const FRAME_BYTES: usize = 4;

pub fn run(
    mode: CaptureMode,
    sink: AudioSink,
    stop: Receiver<()>,
    ready: SyncSender<Result<(), String>>,
) -> Result<(), String> {
    // Keep COM initialized until every audio interface has been dropped.
    let _apartment = match ComApartment::new() {
        Ok(apartment) => apartment,
        Err(error) => {
            let error = describe(error);
            let _ = ready.send(Err(error.clone()));
            return Err(error);
        }
    };
    // Declared after the apartment: Rust drops this session first on every exit.
    let session = match Session::open(mode, &stop, &sink) {
        Ok(session) => session,
        Err(error) => {
            let error = describe(error);
            let _ = ready.send(Err(error.clone()));
            return Err(error);
        }
    };
    // Stop() is valid as soon as start() returns, including before any packet.
    if ready.send(Ok(())).is_err() {
        return Ok(());
    }
    let result = session.capture(&sink, &stop).map_err(describe);
    // Session::drop stops the client on success and every error path.
    result
}

#[derive(Debug)]
enum BackendError {
    Api(windows::core::Error),
    Message(&'static str),
}
type BackendResult<T> = Result<T, BackendError>;

impl From<windows::core::Error> for BackendError {
    fn from(error: windows::core::Error) -> Self {
        Self::Api(error)
    }
}
impl std::fmt::Display for BackendError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Api(error) => write!(
                formatter,
                "WASAPI: {} (HRESULT 0x{:08X})",
                error.message(),
                error.code().0 as u32
            ),
            Self::Message(message) => write!(formatter, "WASAPI: {message}"),
        }
    }
}

fn describe(error: BackendError) -> String {
    error.to_string()
}
fn error(message: &'static str) -> BackendError {
    BackendError::Message(message)
}

fn cancelled(stop: &Receiver<()>, sink: &AudioSink) -> bool {
    !sink.active() || !matches!(stop.try_recv(), Err(TryRecvError::Empty))
}

struct ComApartment;
impl ComApartment {
    fn new() -> BackendResult<Self> {
        // SAFETY: called once on our fresh worker, never on Node's thread.
        unsafe { CoInitializeEx(None, COINIT_MULTITHREADED).ok()? };
        Ok(Self)
    }
}
impl Drop for ComApartment {
    fn drop(&mut self) {
        // SAFETY: balanced with successful initialization on this same thread.
        unsafe { CoUninitialize() };
    }
}

struct Event(HANDLE);
impl Event {
    fn new() -> BackendResult<Self> {
        // SAFETY: unnamed auto-reset event; no borrowed security descriptor.
        unsafe {
            CreateEventW(None, false, false, None)
                .map(Self)
                .map_err(Into::into)
        }
    }
    fn wait(&self) -> BackendResult<()> {
        // SAFETY: this worker owns the event until Session has been dropped.
        match unsafe { WaitForSingleObject(self.0, STOP_POLL.as_millis() as u32) } {
            WAIT_OBJECT_0 | WAIT_TIMEOUT => Ok(()),
            WAIT_FAILED => Err(windows::core::Error::from_thread().into()),
            _ => Err(error("Unexpected audio event wait result")),
        }
    }
}
impl Drop for Event {
    fn drop(&mut self) {
        // SAFETY: unique event ownership, after the audio client is released.
        let _ = unsafe { CloseHandle(self.0) };
    }
}

struct AudioPriority(HANDLE);
impl AudioPriority {
    fn new() -> Option<Self> {
        let mut task_index = 0;
        // SAFETY: registers only this worker; scheduler failure is non-fatal.
        unsafe { AvSetMmThreadCharacteristicsW(w!("Audio"), &mut task_index) }
            .ok()
            .map(Self)
    }
}
impl Drop for AudioPriority {
    fn drop(&mut self) {
        // SAFETY: balanced on the thread that registered this MMCSS task.
        let _ = unsafe { AvRevertMmThreadCharacteristics(self.0) };
    }
}

// windows::core::implement supplies IAgileObject/IMarshal by default. The
// callback shares only a safe channel and immutable owned activation params;
// no audio interface is transferred between threads. Windows retains it until
// activation completes, so a timeout/late callback cannot touch freed state.
#[implement(IActivateAudioInterfaceCompletionHandler)]
struct ActivationHandler {
    completed: SyncSender<()>,
    params: Box<AUDIOCLIENT_ACTIVATION_PARAMS>,
}
impl IActivateAudioInterfaceCompletionHandler_Impl for ActivationHandler_Impl {
    fn ActivateCompleted(
        &self,
        _operation: Ref<'_, IActivateAudioInterfaceAsyncOperation>,
    ) -> windows::core::Result<()> {
        // Never blocks and does not panic if activation was cancelled/timed out.
        let _ = self.completed.try_send(());
        Ok(())
    }
}

fn activate_process(
    pid: u32,
    include: bool,
    stop: &Receiver<()>,
    sink: &AudioSink,
) -> BackendResult<IAudioClient> {
    let (completed, receiver) = mpsc::sync_channel(1);
    let handler = ActivationHandler {
        completed,
        params: Box::new(AUDIOCLIENT_ACTIVATION_PARAMS {
            ActivationType: AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
            Anonymous: AUDIOCLIENT_ACTIVATION_PARAMS_0 {
                ProcessLoopbackParams: AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
                    TargetProcessId: pid,
                    ProcessLoopbackMode: if include {
                        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                    } else {
                        PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
                    },
                },
            },
        }),
    };
    // This is a borrowed PROPVARIANT, not a PropVariantClear-owned allocation.
    // Its blob lives in the COM handler even if our activation wait times out.
    let params = ManuallyDrop::new(PROPVARIANT {
        Anonymous: PROPVARIANT_0 {
            Anonymous: ManuallyDrop::new(PROPVARIANT_0_0 {
                vt: VT_BLOB,
                Anonymous: PROPVARIANT_0_0_0 {
                    blob: BLOB {
                        cbSize: size_of::<AUDIOCLIENT_ACTIVATION_PARAMS>() as u32,
                        pBlobData: ptr::from_ref(handler.params.as_ref()).cast_mut().cast(),
                    },
                },
                ..Default::default()
            }),
        },
    });
    let handler: IActivateAudioInterfaceCompletionHandler = handler.into();
    // SAFETY: ABI-compatible params and interface IID; handler owns blob storage.
    let operation = unsafe {
        ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            &IAudioClient::IID,
            Some(&*params),
            &handler,
        )?
    };
    wait_for_activation(&receiver, ACTIVATION_TIMEOUT, || cancelled(stop, sink))?;
    let mut result = HRESULT(0);
    let mut interface = None;
    // SAFETY: completion has been signalled; outputs are valid initialized cells.
    unsafe { operation.GetActivateResult(&mut result, &mut interface)? };
    result.ok()?;
    Ok(interface
        .ok_or_else(|| error("Audio activation returned no interface"))?
        .cast()?)
}

/// Waiting is independent of COM ownership, so timeout/cancellation are testable
/// without audio hardware. The OS-owned callback can safely outlive this wait.
fn wait_for_activation(
    receiver: &Receiver<()>,
    timeout: Duration,
    mut is_cancelled: impl FnMut() -> bool,
) -> BackendResult<()> {
    let deadline = Instant::now() + timeout;
    loop {
        if is_cancelled() {
            return Err(error("Audio interface activation cancelled"));
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(error(
                "Timed out activating process loopback (requires Windows build 20348+)",
            ));
        }
        match receiver.recv_timeout(STOP_POLL.min(remaining)) {
            Ok(()) => return Ok(()),
            Err(mpsc::RecvTimeoutError::Timeout) => continue,
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                return Err(error("Audio activation callback disconnected"));
            }
        }
    }
}

fn is_silent_i16(bytes: &[u8], threshold: i16) -> bool {
    bytes
        .as_chunks::<2>()
        .0
        .iter()
        .all(|c| i16::from_le_bytes([c[0], c[1]]).saturating_abs() <= threshold)
}

struct Session {
    // Fields drop in declaration order: interfaces before their event handle.
    capture: IAudioCaptureClient,
    client: IAudioClient,
    event: Event,
    _priority: Option<AudioPriority>,
}
impl Session {
    fn open(mode: CaptureMode, stop: &Receiver<()>, sink: &AudioSink) -> BackendResult<Self> {
        let event = Event::new()?;
        let client = match mode {
            CaptureMode::System => {
                // SAFETY: this worker initialized MTA; generated wrappers own refs.
                let enumerator: IMMDeviceEnumerator =
                    unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL)? };
                // SAFETY: valid enumerator and system-defined flow/role.
                let endpoint = unsafe { enumerator.GetDefaultAudioEndpoint(eRender, eConsole)? };
                // SAFETY: no activation params are required for endpoint loopback.
                unsafe { endpoint.Activate::<IAudioClient>(CLSCTX_ALL, None)? }
            }
            CaptureMode::Include(pid) => activate_process(pid, true, stop, sink)?,
            CaptureMode::Exclude(pid) => activate_process(pid, false, stop, sink)?,
        };
        let format = WAVEFORMATEX {
            wFormatTag: WAVE_FORMAT_PCM as u16,
            nChannels: 2,
            nSamplesPerSec: 48_000,
            nAvgBytesPerSec: 192_000,
            nBlockAlign: FRAME_BYTES as u16,
            wBitsPerSample: 16,
            cbSize: 0,
        };
        // SAFETY: format lives through Initialize; WASAPI copies it. Shared
        // AUTOCONVERTPCM fixes the JS contract independently of endpoint format.
        unsafe {
            client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK
                    | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                0,
                0,
                &format,
                None,
            )?;
            client.SetEventHandle(event.0)?;
        }
        // SAFETY: client initialized for capture and returns a referenced service.
        let capture = unsafe { client.GetService::<IAudioCaptureClient>()? };
        let session = Self {
            capture,
            client,
            event,
            _priority: AudioPriority::new(),
        };
        // SAFETY: fully initialized session. Drop handles partial Start failure.
        unsafe { session.client.Start()? };
        Ok(session)
    }

    fn capture(&self, sink: &AudioSink, stop: &Receiver<()>) -> BackendResult<()> {
        while !cancelled(stop, sink) {
            self.event.wait()?;
            // Drain even after a timeout: device invalidation can stop events.
            // Limit each pass so a continuously busy device cannot starve stop.
            for _ in 0..128 {
                if cancelled(stop, sink) {
                    return Ok(());
                }
                // SAFETY: this worker owns the active capture client.
                if unsafe { self.capture.GetNextPacketSize()? } == 0 {
                    break;
                }
                let packet = Packet::read(&self.capture)?;

                if packet.is_silent() {
                    // Return the driver's buffer before enqueueing to another thread.
                    packet.release()?;
                    continue;
                }

                let bytes = packet.bytes()?;
                // Return the driver's buffer before enqueueing to another thread.
                packet.release()?;

                if bytes.is_empty() || is_silent_i16(&bytes, 4) {
                    continue;
                }

                if !sink.emit(bytes) {
                    return Ok(());
                }
            }
        }
        Ok(())
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        // SAFETY: same thread owns client; Stop also runs after any capture error.
        let _ = unsafe { self.client.Stop() };
    }
}

/// A borrowed WASAPI packet. Release runs exactly once, including early errors.
struct Packet<'a> {
    client: &'a IAudioCaptureClient,
    data: *mut u8,
    frames: u32,
    flags: u32,
}
impl<'a> Packet<'a> {
    fn read(client: &'a IAudioCaptureClient) -> BackendResult<Self> {
        let mut data = ptr::null_mut();
        let mut frames = 0;
        let mut flags = 0;
        // SAFETY: output cells are live and no other packet is held by this worker.
        unsafe { client.GetBuffer(&mut data, &mut frames, &mut flags, None, None)? };
        Ok(Self {
            client,
            data,
            frames,
            flags,
        })
    }
    fn bytes(&self) -> BackendResult<Vec<u8>> {
        let bytes = (self.frames as usize)
            .checked_mul(FRAME_BYTES)
            .filter(|bytes| *bytes <= isize::MAX as usize)
            .ok_or_else(|| error("Audio packet size overflow"))?;
        if self.flags & AUDCLNT_BUFFERFLAGS_SILENT.0 as u32 != 0 {
            // The pointer need not be readable for a SILENT packet.
            return Ok(vec![0; bytes]);
        }
        if bytes == 0 {
            return Ok(Vec::new());
        }
        if self.data.is_null() {
            return Err(error("WASAPI returned a null non-silent packet"));
        }
        // SAFETY: GetBuffer guarantees frames*block-align readable bytes until
        // ReleaseBuffer. Packet borrows the client; no release occurs until copy.
        Ok(unsafe { std::slice::from_raw_parts(self.data, bytes) }.to_vec())
    }
    fn release(mut self) -> BackendResult<()> {
        let frames = std::mem::take(&mut self.frames);
        if frames == 0 {
            return Ok(());
        }
        // SAFETY: matching successful GetBuffer on the same worker exactly once.
        unsafe { self.client.ReleaseBuffer(frames) }.map_err(Into::into)
    }

    fn is_silent(&self) -> bool {
        self.flags & AUDCLNT_BUFFERFLAGS_SILENT.0 as u32 != 0
    }
}
impl Drop for Packet<'_> {
    fn drop(&mut self) {
        if self.frames != 0 {
            // SAFETY: fallback for early returns; release() sets frames to zero.
            let _ = unsafe { self.client.ReleaseBuffer(self.frames) };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    };

    #[implement(IAudioCaptureClient)]
    struct FakeCapture {
        data: Vec<u8>,
        frames: u32,
        flags: u32,
        null: bool,
        fail_read: bool,
        fail_release: bool,
        releases: Arc<AtomicUsize>,
    }
    impl IAudioCaptureClient_Impl for FakeCapture_Impl {
        fn GetBuffer(
            &self,
            data: *mut *mut u8,
            frames: *mut u32,
            flags: *mut u32,
            _position: *mut u64,
            _qpc: *mut u64,
        ) -> windows::core::Result<()> {
            // SAFETY: tests call only through Packet::read's valid out-pointers;
            // FakeCapture owns immutable storage until the interface is dropped.
            unsafe {
                data.write(if self.null {
                    ptr::null_mut()
                } else {
                    self.data.as_ptr().cast_mut()
                });
                frames.write(self.frames);
                flags.write(self.flags);
            }
            if self.fail_read {
                Err(windows::core::Error::from_hresult(HRESULT(
                    0x80004005_u32 as i32,
                )))
            } else {
                Ok(())
            }
        }
        fn ReleaseBuffer(&self, _frames: u32) -> windows::core::Result<()> {
            self.releases.fetch_add(1, Ordering::SeqCst);
            if self.fail_release {
                Err(windows::core::Error::from_hresult(HRESULT(
                    0x80004005_u32 as i32,
                )))
            } else {
                Ok(())
            }
        }
        fn GetNextPacketSize(&self) -> windows::core::Result<u32> {
            Ok(self.frames)
        }
    }

    fn fake(
        data: Vec<u8>,
        null: bool,
        silent: bool,
        fail_read: bool,
        fail_release: bool,
    ) -> (IAudioCaptureClient, Arc<AtomicUsize>) {
        let releases = Arc::new(AtomicUsize::new(0));
        let capture = FakeCapture {
            data,
            frames: 2,
            flags: if silent {
                AUDCLNT_BUFFERFLAGS_SILENT.0 as u32
            } else {
                0
            },
            null,
            fail_read,
            fail_release,
            releases: releases.clone(),
        };
        (capture.into(), releases)
    }

    #[test]
    fn silent_null_packet_produces_full_length_zeroes() {
        let (client, releases) = fake(Vec::new(), true, true, false, false);
        let packet = Packet::read(&client).unwrap();
        assert_eq!(packet.bytes().unwrap(), vec![0; 8]);
        packet.release().unwrap();
        assert_eq!(releases.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn quiet_pcm_is_preserved_byte_for_byte() {
        let quiet = vec![1, 0, 255, 255, 0, 0, 1, 0];
        let (client, releases) = fake(quiet.clone(), false, false, false, false);
        let packet = Packet::read(&client).unwrap();
        assert_eq!(packet.bytes().unwrap(), quiet);
        drop(packet);
        assert_eq!(releases.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn bad_packet_still_releases_once() {
        let (client, releases) = fake(Vec::new(), true, false, false, false);
        let packet = Packet::read(&client).unwrap();
        assert!(packet.bytes().is_err());
        drop(packet);
        assert_eq!(releases.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn failed_getbuffer_is_not_released() {
        let (client, releases) = fake(Vec::new(), true, false, true, false);
        assert!(Packet::read(&client).is_err());
        assert_eq!(releases.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn failed_release_is_not_retried() {
        let (client, releases) = fake(Vec::new(), true, true, false, true);
        assert!(Packet::read(&client).unwrap().release().is_err());
        assert_eq!(releases.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn activation_callback_after_waiter_dropped_is_harmless() {
        let (completed, receiver) = mpsc::sync_channel(1);
        drop(receiver);
        let handler: IActivateAudioInterfaceCompletionHandler = ActivationHandler {
            completed,
            params: Box::default(),
        }
        .into();
        // SAFETY: our callback ignores the optional operation; simulate a late
        // callback after timeout without requiring Windows audio hardware.
        unsafe { handler.ActivateCompleted(None::<&IActivateAudioInterfaceAsyncOperation>) }
            .unwrap();
    }
    #[test]
    fn activation_wait_times_out_without_a_callback() {
        let (_sender, receiver) = mpsc::sync_channel(1);
        let started = Instant::now();
        assert!(wait_for_activation(&receiver, Duration::from_millis(1), || false).is_err());
        assert!(started.elapsed() < Duration::from_secs(1));
    }

    #[test]
    fn activation_wait_cancels_before_completion() {
        let (_sender, receiver) = mpsc::sync_channel(1);
        let error = wait_for_activation(&receiver, Duration::from_secs(1), || true).unwrap_err();
        assert!(error.to_string().contains("cancelled"));
    }

    #[test]
    fn activation_wait_accepts_completion() {
        let (sender, receiver) = mpsc::sync_channel(1);
        sender.send(()).unwrap();
        wait_for_activation(&receiver, Duration::from_secs(1), || false).unwrap();
    }

    #[test]
    fn activation_wait_reports_disconnected_callback() {
        let (sender, receiver) = mpsc::sync_channel(1);
        drop(sender);
        assert!(
            wait_for_activation(&receiver, Duration::from_secs(1), || false)
                .unwrap_err()
                .to_string()
                .contains("disconnected")
        );
    }
}
