//! Platform-neutral capture ownership and bounded delivery. Native handles never leave a worker.
use std::{
    collections::VecDeque,
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, Sender},
        Arc, Mutex, MutexGuard,
    },
    thread::{self, JoinHandle},
    time::Duration,
};

pub const QUEUE_CAPACITY: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CaptureMode {
    System,
    Include(u32),
    Exclude(u32),
}

// No user code runs while a lock is held. Recovering a poisoned lock permits shutdown
// even if a worker panics; the worker's panic is reported through the join result.
pub fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[derive(Default)]
struct Pending {
    packets: VecDeque<Vec<u8>>,
    notified: bool,
}

pub struct Mailbox {
    active: AtomicBool,
    pending: Mutex<Pending>,
    error: Mutex<Option<String>>,
}

impl Mailbox {
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            active: AtomicBool::new(true),
            pending: Mutex::new(Pending::default()),
            error: Mutex::new(None),
        })
    }

    pub fn active(&self) -> bool {
        self.active.load(Ordering::Acquire)
    }

    /// Returns whether a single coalesced JS wakeup must be scheduled.
    fn push(&self, packet: Vec<u8>) -> bool {
        let mut pending = lock(&self.pending);
        if !self.active() || packet.is_empty() || pending.packets.len() == QUEUE_CAPACITY {
            return false;
        }
        pending.packets.push_back(packet);
        let notify = !pending.notified;
        pending.notified = true;
        notify
    }

    pub fn drain(&self) -> VecDeque<Vec<u8>> {
        let mut pending = lock(&self.pending);
        pending.notified = false;
        std::mem::take(&mut pending.packets)
    }

    pub fn close(&self) {
        self.active.store(false, Ordering::Release);
        lock(&self.pending).packets.clear();
    }

    pub fn error(&self) -> Option<String> {
        lock(&self.error).clone()
    }
}

#[derive(Clone)]
pub struct AudioSink {
    mailbox: Arc<Mailbox>,
    wake: Arc<dyn Fn() -> bool + Send + Sync>,
}

impl AudioSink {
    pub fn new(mailbox: Arc<Mailbox>, wake: impl Fn() -> bool + Send + Sync + 'static) -> Self {
        Self {
            mailbox,
            wake: Arc::new(wake),
        }
    }

    pub fn active(&self) -> bool {
        self.mailbox.active()
    }

    /// Drops new packets when full. Capture never waits for JavaScript to consume data.
    pub fn emit(&self, bytes: Vec<u8>) -> bool {
        if self.mailbox.push(bytes) && !(self.wake)() {
            self.mailbox.close();
        }
        self.active()
    }
}

struct Worker {
    stop: Sender<()>,
    finished: Receiver<()>,
    thread: JoinHandle<Result<(), String>>,
}

pub struct Session {
    mailbox: Arc<Mailbox>,
    worker: Mutex<Option<Worker>>,
}

impl Session {
    pub fn start(mode: CaptureMode, sink: AudioSink) -> Result<Arc<Self>, String> {
        Self::start_worker(sink, move |sink, stopping, ready| {
            #[cfg(target_os = "linux")]
            {
                crate::linux::run(mode, sink, stopping, ready)
            }
            #[cfg(target_os = "windows")]
            {
                crate::windows::run(mode, sink, stopping, ready)
            }
        })
    }

    fn start_worker(
        sink: AudioSink,
        backend: impl FnOnce(
                AudioSink,
                Receiver<()>,
                mpsc::SyncSender<Result<(), String>>,
            ) -> Result<(), String>
            + Send
            + 'static,
    ) -> Result<Arc<Self>, String> {
        let mailbox = sink.mailbox.clone();
        let (stop, stopping) = mpsc::channel();
        let (ready, started) = mpsc::sync_channel(1);
        let (finished, completion) = mpsc::channel();
        let worker_mailbox = mailbox.clone();
        let thread = thread::Builder::new()
            .name("loopback-capture".into())
            .spawn(move || {
                // A backend panic must not leave delivery active or escape into
                // Node. No backend state is reused after unwinding this worker.
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    backend(sink, stopping, ready)
                }))
                .unwrap_or_else(|_| Err("Audio capture worker panicked".into()));
                if let Err(message) = &result {
                    *lock(&worker_mailbox.error) = Some(message.clone());
                }
                worker_mailbox.close();
                let _ = finished.send(());
                result
            })
            .map_err(|e| format!("Failed to start capture worker: {e}"))?;
        let session = Arc::new(Self {
            mailbox,
            worker: Mutex::new(Some(Worker {
                stop,
                finished: completion,
                thread,
            })),
        });
        match started.recv_timeout(Duration::from_secs(10)) {
            Ok(Ok(())) => Ok(session),
            result => {
                let error = match result {
                    Ok(Err(message)) => message,
                    Err(mpsc::RecvTimeoutError::Timeout) => {
                        "Timed out starting audio capture".into()
                    }
                    _ => "Audio capture worker exited during startup".into(),
                };
                let stopped = session.stop();
                Err(session
                    .mailbox
                    .error()
                    .or_else(|| stopped.err())
                    .unwrap_or(error))
            }
        }
    }

    /// Idempotent; closing delivery first also suppresses callbacks queued before stop.
    pub fn stop(&self) -> Result<(), String> {
        self.stop_with_timeout(Duration::from_secs(6))
    }

    fn stop_with_timeout(&self, timeout: Duration) -> Result<(), String> {
        self.mailbox.close();
        let worker = lock(&self.worker).take();
        if let Some(worker) = worker {
            let _ = worker.stop.send(());
            if worker.finished.recv_timeout(timeout) == Err(mpsc::RecvTimeoutError::Timeout) {
                // A stuck system call cannot safely be killed. Detach its worker;
                // it owns every native resource and closed delivery cannot reach JS.
                let error = "Timed out stopping audio capture; worker is finishing in background"
                    .to_owned();
                *lock(&self.mailbox.error) = Some(error.clone());
                return Err(error);
            }
            worker
                .thread
                .join()
                .map_err(|_| "Audio capture worker panicked".to_owned())??;
        }
        Ok(())
    }

    pub fn error(&self) -> Option<String> {
        self.mailbox.error()
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    #[test]
    fn bounds_audio_and_coalesces_wakeups_without_blocking() {
        let mailbox = Mailbox::new();
        let calls = Arc::new(AtomicUsize::new(0));
        let wake_calls = calls.clone();
        let sink = AudioSink::new(mailbox.clone(), move || {
            wake_calls.fetch_add(1, Ordering::Relaxed);
            true
        });
        for value in 0..QUEUE_CAPACITY * 2 {
            assert!(sink.emit(vec![value as u8; 4]));
        }
        assert_eq!(calls.load(Ordering::Relaxed), 1);
        let packets = mailbox.drain();
        assert_eq!(packets.len(), QUEUE_CAPACITY);
        assert_eq!(packets.back().unwrap(), &[63; 4]);
        sink.emit(vec![255; 4]);
        assert_eq!(calls.load(Ordering::Relaxed), 2);
    }

    #[test]
    fn closing_discards_pending_audio_and_rejects_further_delivery() {
        let mailbox = Mailbox::new();
        let sink = AudioSink::new(mailbox.clone(), || true);
        sink.emit(vec![1; 4]);
        mailbox.close();
        assert!(!sink.emit(vec![2; 4]));
        assert!(mailbox.drain().is_empty());
    }

    #[test]
    fn failed_wakeup_closes_delivery_and_frees_queued_audio() {
        let mailbox = Mailbox::new();
        let sink = AudioSink::new(mailbox.clone(), || false);
        assert!(!sink.emit(vec![1; 4]));
        assert!(mailbox.drain().is_empty());
    }

    #[test]
    fn immediate_stop_closes_delivery_before_notifying_worker_and_is_idempotent() {
        let mailbox = Mailbox::new();
        let sink = AudioSink::new(mailbox, || true);
        let session = Session::start_worker(sink, |sink, stop, ready| {
            ready.send(Ok(())).unwrap();
            stop.recv().unwrap();
            assert!(!sink.emit(vec![1; 4]));
            Ok(())
        })
        .unwrap();
        session.stop().unwrap();
        session.stop().unwrap();
        assert_eq!(session.error(), None);
    }

    #[test]
    fn startup_failure_releases_worker_and_preserves_diagnostic() {
        let mailbox = Mailbox::new();
        let sink = AudioSink::new(mailbox.clone(), || true);
        let result = Session::start_worker(sink, |_, _, ready| {
            let message = "No audio endpoint".to_owned();
            ready.send(Err(message.clone())).unwrap();
            Err(message)
        });
        assert_eq!(result.err().as_deref(), Some("No audio endpoint"));
        assert!(!mailbox.active());
    }

    #[test]
    fn worker_panic_closes_delivery_and_is_reported_as_an_error() {
        let mailbox = Mailbox::new();
        let sink = AudioSink::new(mailbox.clone(), || true);
        let session = Session::start_worker(sink, |_, stop, ready| {
            ready.send(Ok(())).unwrap();
            stop.recv().unwrap();
            panic!("simulated backend panic");
        })
        .unwrap();
        assert_eq!(session.stop().unwrap_err(), "Audio capture worker panicked");
        assert_eq!(
            session.error().as_deref(),
            Some("Audio capture worker panicked")
        );
        assert!(!mailbox.active());
    }

    #[test]
    fn unresponsive_backend_cannot_block_stop_indefinitely_or_deliver_more_audio() {
        let mailbox = Mailbox::new();
        let sink = AudioSink::new(mailbox.clone(), || true);
        let (release, wait) = mpsc::channel();
        let (finished, done) = mpsc::channel();
        let session = Session::start_worker(sink, move |sink, _, ready| {
            ready.send(Ok(())).unwrap();
            wait.recv().unwrap();
            assert!(!sink.emit(vec![1; 4]));
            finished.send(()).unwrap();
            Ok(())
        })
        .unwrap();
        assert!(session
            .stop_with_timeout(Duration::from_millis(1))
            .unwrap_err()
            .contains("Timed out stopping"));
        assert!(!mailbox.active());
        release.send(()).unwrap();
        done.recv_timeout(Duration::from_secs(1)).unwrap();
        session.stop().unwrap();
    }
}
