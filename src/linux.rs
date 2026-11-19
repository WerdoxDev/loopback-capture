//! PipeWire graph and stream ownership stays entirely on the capture worker.
//! Callbacks enqueue owned graph updates; only audio packets cross to Node.

#[path = "process_tree.rs"]
mod process_tree;

use crate::capture::{AudioSink, CaptureMode};
use pipewire as pw;
use process_tree::{read_identity, ProcessTree};
use pw::{
    properties::{properties, PropertiesBox},
    proxy::ProxyT,
    spa,
    types::ObjectType,
};
use std::{
    collections::{BTreeSet, HashMap},
    io::Write,
    sync::mpsc::{self, Receiver, Sender, SyncSender, TryRecvError},
    time::{Duration, Instant},
};

type Properties = HashMap<String, String>;
type LinkKey = (u32, u32);

fn copy_properties(props: &spa::utils::dict::DictRef) -> Properties {
    props
        .iter()
        .map(|(key, value)| (key.to_owned(), value.to_owned()))
        .collect()
}

fn is_silent_i16(bytes: &[u8], threshold: i16) -> bool {
    bytes
        .as_chunks::<2>()
        .0
        .iter()
        .all(|c| i16::from_ne_bytes([c[0], c[1]]).saturating_abs() <= threshold)
}

enum Event {
    Global(pw::registry::GlobalObject<PropertiesBox>),
    Removed(u32),
    ClientInfo(u32, Properties),
    NodeInfo(u32, Properties),
    LinkFailed(LinkKey),
    Error(String),
}

// Listeners must be destroyed before their associated proxies. Field order
// expresses this directly, without raw listener pointers or custom Drop code.
struct Client {
    _listener: pw::client::ClientListener,
    _proxy: pw::client::Client,
    props: Properties,
    received_info: bool,
    pulse_server: bool,
}

struct Node {
    _listener: pw::node::NodeListener,
    _proxy: pw::node::Node,
    props: Properties,
    discovered: Instant,
    unknown_reported: bool,
}

struct Port {
    node: u32,
    channel: String,
    input: bool,
}

struct Link {
    _listener: pw::proxy::ProxyListener,
    _proxy: pw::link::Link,
    retry_after: Option<Instant>,
}

struct Connection {
    clients: HashMap<u32, Client>,
    nodes: HashMap<u32, Node>,
    links: HashMap<LinkKey, Link>,
    ports: HashMap<u32, Port>,
    _registry_listener: Option<pw::registry::Listener>,
    _stream_listener: pw::stream::StreamListener<()>,
    _core_listener: pw::core::Listener,
    registry: Option<pw::registry::RegistryRc>,
    stream: pw::stream::StreamRc,
    core: pw::core::CoreRc,
    events: Receiver<Event>,
    sender: Sender<Event>,
}

impl Connection {
    fn new(
        context: &pw::context::ContextRc,
        mode: CaptureMode,
        sink: AudioSink,
    ) -> Result<Self, String> {
        let core = context
            .connect_rc(None)
            .map_err(|error| format!("Failed to connect to PipeWire: {error}"))?;
        let (sender, events) = mpsc::channel();
        let errors = sender.clone();
        let core_listener = core
            .add_listener_local()
            .error(move |id, _, result, message| {
                if id == pw::core::PW_ID_CORE && result < 0 {
                    let _ = errors.send(Event::Error(message.to_owned()));
                }
            })
            .register();
        let filtered = mode != CaptureMode::System;
        let registry = if filtered {
            Some(
                core.get_registry_rc()
                    .map_err(|error| format!("Failed to get PipeWire registry: {error}"))?,
            )
        } else {
            None
        };
        let registry_listener = registry.as_ref().map(|registry| {
            let globals = sender.clone();
            let removed = sender.clone();
            registry
                .add_listener_local()
                .global(move |global| {
                    let _ = globals.send(Event::Global(global.to_owned()));
                })
                .global_remove(move |id| {
                    let _ = removed.send(Event::Removed(id));
                })
                .register()
        });
        let mut props = properties! {
            "media.type" => "Audio",
            "media.category" => "Capture",
            "media.role" => "Screen",
            "node.name" => "loopback-capture",
            "node.description" => "Loopback Capture",
        };
        if filtered {
            // Never allow a session manager to attach filtered capture to an
            // unfiltered sink or microphone. Existing playback links survive.
            props.insert("node.autoconnect", "false");
            props.insert("node.always-process", "true");
            props.insert(
                "adapter.auto-port-config",
                "{ mode = dsp monitor = false position = preserve }",
            );
        } else {
            props.insert("stream.capture.sink", "true");
        }
        let stream = pw::stream::StreamRc::new(core.clone(), "Loopback capture", props)
            .map_err(|error| format!("Failed to create PipeWire stream: {error}"))?;
        let stream_errors = sender.clone();
        let stream_listener = stream
            .add_local_listener_with_user_data(())
            .state_changed(move |_, _, _, state| {
                if let pw::stream::StreamState::Error(error) = state {
                    let _ = stream_errors.send(Event::Error(error));
                } else if state == pw::stream::StreamState::Unconnected {
                    let _ =
                        stream_errors.send(Event::Error("PipeWire capture disconnected".into()));
                }
            })
            .process(move |stream, _| {
                while let Some(mut buffer) = stream.dequeue_buffer() {
                    let Some(plane) = buffer.datas_mut().first_mut() else {
                        continue;
                    };
                    let offset = plane.chunk().offset() as usize;
                    let length = plane.chunk().size() as usize;

                    if let Some(data) = plane.data() {
                        let offset = offset.min(data.len());
                        let end = offset.saturating_add(length).min(data.len());
                        let bytes = &data[offset..end];

                        // Check the raw samples before downmixing, so silent
                        // packets skip the downmix work entirely.
                        if bytes.is_empty() || is_silent_i16(bytes, 4) {
                            continue;
                        }

                        let audio = if filtered {
                            downmix(bytes)
                        } else {
                            bytes[..bytes.len() - bytes.len() % 4].to_vec()
                        };
                        if !audio.is_empty() && !sink.emit(audio) {
                            break;
                        }
                    }
                }
            })
            .register()
            .map_err(|error| format!("Failed to register PipeWire stream: {error}"))?;
        let format = audio_format(filtered)?;
        let pod = spa::pod::Pod::from_bytes(&format).ok_or("Invalid PipeWire audio format")?;
        let flags = pw::stream::StreamFlags::MAP_BUFFERS
            | if filtered {
                pw::stream::StreamFlags::empty()
            } else {
                pw::stream::StreamFlags::AUTOCONNECT
            };
        stream
            .connect(spa::utils::Direction::Input, None, flags, &mut [pod])
            .map_err(|error| format!("Failed to connect PipeWire stream: {error}"))?;
        Ok(Self {
            clients: HashMap::new(),
            nodes: HashMap::new(),
            links: HashMap::new(),
            ports: HashMap::new(),
            _registry_listener: registry_listener,
            _stream_listener: stream_listener,
            _core_listener: core_listener,
            registry,
            stream,
            core,
            events,
            sender,
        })
    }

    fn ready(&self) -> bool {
        matches!(
            self.stream.state(),
            pw::stream::StreamState::Paused | pw::stream::StreamState::Streaming
        )
    }

    fn dispatch(&mut self) -> Result<bool, String> {
        let mut changed = false;
        while let Ok(event) = self.events.try_recv() {
            changed = true;
            match event {
                Event::Global(global) => self.global(global),
                Event::Removed(id) => {
                    self.ports.remove(&id);
                    self.nodes.remove(&id);
                    self.clients.remove(&id);
                }
                Event::ClientInfo(id, props) => {
                    if let Some(client) = self.clients.get_mut(&id) {
                        client.props.extend(props);
                        client.received_info = true;
                        client.pulse_server = is_pulse_server(&client.props);
                    }
                }
                Event::NodeInfo(id, props) => {
                    if let Some(node) = self.nodes.get_mut(&id) {
                        node.props.extend(props);
                    }
                }
                Event::LinkFailed(key) => {
                    if let Some(link) = self.links.get_mut(&key) {
                        link.retry_after = Some(Instant::now() + Duration::from_secs(1));
                    }
                }
                Event::Error(error) => return Err(error),
            }
        }
        Ok(changed)
    }

    fn global(&mut self, global: pw::registry::GlobalObject<PropertiesBox>) {
        let Some(registry) = &self.registry else {
            return;
        };
        let Some(props) = global.props.as_ref() else {
            return;
        };
        let id = global.id;
        match global.type_ {
            ObjectType::Client => {
                let Ok(proxy) = registry.bind::<pw::client::Client, _>(&global) else {
                    return;
                };
                let sender = self.sender.clone();
                let listener = proxy
                    .add_listener_local()
                    .info(move |info| {
                        let props = info.props().map(copy_properties).unwrap_or_default();
                        let _ = sender.send(Event::ClientInfo(id, props));
                    })
                    .register();
                self.clients.insert(
                    id,
                    Client {
                        _listener: listener,
                        _proxy: proxy,
                        props: copy_properties(props.as_ref()),
                        received_info: false,
                        pulse_server: false,
                    },
                );
            }
            ObjectType::Node if props.get("media.class") == Some("Stream/Output/Audio") => {
                let Ok(proxy) = registry.bind::<pw::node::Node, _>(&global) else {
                    return;
                };
                let sender = self.sender.clone();
                let listener = proxy
                    .add_listener_local()
                    .info(move |info| {
                        let props = info.props().map(copy_properties).unwrap_or_default();
                        let _ = sender.send(Event::NodeInfo(id, props));
                    })
                    .register();
                self.nodes.insert(
                    id,
                    Node {
                        _listener: listener,
                        _proxy: proxy,
                        props: copy_properties(props.as_ref()),
                        discovered: Instant::now(),
                        unknown_reported: false,
                    },
                );
            }
            ObjectType::Port => {
                if props.get("port.monitor") == Some("true")
                    || props.get("format.dsp") != Some("32 bit float mono audio")
                {
                    return;
                }
                let Some(node) = props.get("node.id").and_then(|id| id.parse().ok()) else {
                    return;
                };
                let (Some(direction), Some(channel)) =
                    (props.get("port.direction"), props.get("audio.channel"))
                else {
                    return;
                };
                self.ports.insert(
                    id,
                    Port {
                        node,
                        channel: channel.to_owned(),
                        input: direction == "in",
                    },
                );
            }
            _ => {}
        }
    }

    fn refresh_links(&mut self, mode: CaptureMode, tree: &mut ProcessTree) {
        let stream_id = self.stream.node_id();
        if stream_id == pw::constants::ID_ANY || !self.ready() {
            return;
        }
        let mut selected = BTreeSet::new();
        for (&id, node) in &mut self.nodes {
            let client = read_id(&node.props, "client.id").and_then(|id| self.clients.get(&id));
            let pid = client.and_then(|client| {
                owner_pid(
                    &node.props,
                    &client.props,
                    client.received_info,
                    client.pulse_server,
                )
            });
            let Some(pid) = pid.filter(|pid| read_identity(*pid).is_some()) else {
                if !node.unknown_reported && node.discovered.elapsed() > Duration::from_secs(2) {
                    eprintln!("loopback-capture: cannot identify the owner of playback node {id}; skipping it during filtered capture");
                    node.unknown_reported = true;
                }
                continue;
            };
            if tree.contains(pid) == matches!(mode, CaptureMode::Include(_)) {
                selected.insert(id);
            }
        }
        let mut desired = BTreeSet::new();
        for (&output_id, output) in &self.ports {
            if output.input || !selected.contains(&output.node) {
                continue;
            }
            for (&input_id, input) in &self.ports {
                if input.input
                    && input.node == stream_id
                    && channels_match(&output.channel, &input.channel)
                {
                    desired.insert((output_id, input_id));
                }
            }
        }
        self.links.retain(|key, _| desired.contains(key));
        for key in desired {
            if let Some(link) = self.links.get(&key) {
                if link.retry_after.is_none_or(|retry| Instant::now() < retry) {
                    continue;
                }
            }
            self.links.remove(&key);
            let props = properties! {
                "link.output.port" => key.0.to_string(),
                "link.input.port" => key.1.to_string(),
                "object.linger" => "false",
            };
            let Ok(proxy) = self
                .core
                .create_object::<pw::link::Link>("link-factory", &props)
            else {
                continue;
            };
            let errors = self.sender.clone();
            let removed = self.sender.clone();
            let listener = proxy
                .upcast_ref()
                .add_listener_local()
                .error(move |_, _, message| {
                    // Logging must not panic through PipeWire's C callback.
                    let _ = writeln!(
                        std::io::stderr().lock(),
                        "loopback-capture: playback link failed: {message}"
                    );
                    let _ = errors.send(Event::LinkFailed(key));
                })
                .removed(move || {
                    let _ = removed.send(Event::LinkFailed(key));
                })
                .register();
            self.links.insert(
                key,
                Link {
                    _listener: listener,
                    _proxy: proxy,
                    retry_after: None,
                },
            );
        }
    }
}

fn audio_format(filtered: bool) -> Result<Vec<u8>, String> {
    let mut info = spa::param::audio::AudioInfoRaw::new();
    info.set_format(if filtered {
        spa::param::audio::AudioFormat::F32LE
    } else {
        spa::param::audio::AudioFormat::S16LE
    });
    info.set_rate(48_000);
    info.set_channels(if filtered { 8 } else { 2 });
    let mut position = [0; 64];
    position[..8].copy_from_slice(&[
        spa::sys::SPA_AUDIO_CHANNEL_FL,
        spa::sys::SPA_AUDIO_CHANNEL_FR,
        spa::sys::SPA_AUDIO_CHANNEL_FC,
        spa::sys::SPA_AUDIO_CHANNEL_LFE,
        spa::sys::SPA_AUDIO_CHANNEL_RL,
        spa::sys::SPA_AUDIO_CHANNEL_RR,
        spa::sys::SPA_AUDIO_CHANNEL_SL,
        spa::sys::SPA_AUDIO_CHANNEL_SR,
    ]);
    info.set_position(position);
    let object = spa::pod::Object {
        type_: spa::utils::SpaTypes::ObjectParamFormat.as_raw(),
        id: spa::param::ParamType::EnumFormat.as_raw(),
        properties: info.into(),
    };
    spa::pod::serialize::PodSerializer::serialize(
        std::io::Cursor::new(Vec::new()),
        &spa::pod::Value::Object(object),
    )
    .map(|(serializer, _)| serializer.into_inner())
    .map_err(|error| format!("Failed to serialize PipeWire audio format: {error}"))
}

fn read_id(props: &Properties, key: &str) -> Option<u32> {
    props.get(key)?.parse().ok()
}

fn read_pid(props: &Properties, key: &str) -> Option<u32> {
    read_id(props, key).filter(|pid| *pid > 0 && *pid <= i32::MAX as u32)
}

fn is_pulse_server(props: &Properties) -> bool {
    props
        .get("application.process.binary")
        .is_some_and(|name| name == "pipewire-pulse")
        || read_pid(props, "pipewire.sec.pid")
            .and_then(|pid| std::fs::read_to_string(format!("/proc/{pid}/comm")).ok())
            .is_some_and(|name| name.trim_end() == "pipewire-pulse")
}

fn owner_pid(
    node: &Properties,
    client: &Properties,
    received_info: bool,
    pulse_server: bool,
) -> Option<u32> {
    if !received_info {
        return None;
    }
    if let Some(pid) = read_pid(node, "application.process.id") {
        if pulse_server && Some(pid) == read_pid(client, "pipewire.sec.pid") {
            return None;
        }
        return Some(pid);
    }
    if pulse_server {
        return None;
    }
    read_pid(client, "pipewire.sec.pid").or_else(|| read_pid(client, "application.process.id"))
}

fn channels_match(output: &str, input: &str) -> bool {
    output == input
        || matches!(
            (output, input),
            ("MONO", "FL" | "FR")
                | ("FLC" | "AUX0", "FL")
                | ("FRC" | "AUX1", "FR")
                | ("RC", "FC")
                | ("LFE2", "LFE")
        )
}

fn pcm16(sample: f32) -> i16 {
    if sample.is_finite() {
        (sample.clamp(-1.0, 1.0) * 32768.0)
            .round()
            .clamp(-32768.0, 32767.0) as i16
    } else {
        0
    }
}

fn downmix(bytes: &[u8]) -> Vec<u8> {
    let mut pcm = Vec::with_capacity(bytes.len() / 8);
    for frame in bytes.as_chunks::<32>().0 {
        let mut samples = [0.0; 8];
        for (sample, bytes) in samples.iter_mut().zip(frame.as_chunks::<4>().0) {
            *sample = f32::from_le_bytes(*bytes);
        }
        let surround = std::f32::consts::FRAC_1_SQRT_2;
        let center = samples[2] * surround + samples[3] * 0.5;
        pcm.extend_from_slice(
            &pcm16(samples[0] + center + surround * (samples[4] + samples[6])).to_le_bytes(),
        );
        pcm.extend_from_slice(
            &pcm16(samples[1] + center + surround * (samples[5] + samples[7])).to_le_bytes(),
        );
    }
    pcm
}

pub fn run(
    mode: CaptureMode,
    sink: AudioSink,
    stop: Receiver<()>,
    ready: SyncSender<Result<(), String>>,
) -> Result<(), String> {
    let mut tree = match mode {
        CaptureMode::System => None,
        CaptureMode::Include(pid) | CaptureMode::Exclude(pid) => match ProcessTree::new(pid) {
            Ok(tree) => Some(tree),
            Err(error) => {
                let _ = ready.send(Err(error.clone()));
                return Err(error);
            }
        },
    };
    pw::init();
    let environment = (|| {
        let mainloop = pw::main_loop::MainLoopRc::new(None).map_err(|error| error.to_string())?;
        let context =
            pw::context::ContextRc::new(&mainloop, None).map_err(|error| error.to_string())?;
        Ok::<_, String>((mainloop, context))
    })();
    let (mainloop, context) = match environment {
        Ok(environment) => environment,
        Err(error) => {
            let _ = ready.send(Err(error.clone()));
            return Err(error);
        }
    };
    let mut startup = Some(ready);
    loop {
        let mut connection = match Connection::new(&context, mode, sink.clone()) {
            Ok(connection) => connection,
            Err(error) => {
                if let Some(ready) = startup.take() {
                    let _ = ready.send(Err(error.clone()));
                    return Err(error);
                }
                if !matches!(
                    stop.recv_timeout(Duration::from_secs(1)),
                    Err(mpsc::RecvTimeoutError::Timeout)
                ) || !sink.active()
                {
                    return Ok(());
                }
                continue;
            }
        };
        let connected = Instant::now();
        let mut last_refresh = Instant::now();
        let mut last_links = Instant::now() - Duration::from_millis(250);
        let failure = loop {
            if !sink.active() || !matches!(stop.try_recv(), Err(TryRecvError::Empty)) {
                return Ok(());
            }
            mainloop.loop_().iterate(Duration::from_millis(25));
            let graph_changed = match connection.dispatch() {
                Ok(changed) => changed,
                Err(error) => break error,
            };
            if let Some(tree) = &mut tree {
                if last_refresh.elapsed() >= Duration::from_secs(1) {
                    tree.refresh();
                    last_refresh = Instant::now();
                }
                if graph_changed || last_links.elapsed() >= Duration::from_millis(250) {
                    connection.refresh_links(mode, tree);
                    last_links = Instant::now();
                }
            }
            if connection.ready() {
                if let Some(ready) = startup.take() {
                    let _ = ready.send(Ok(()));
                }
            } else if connected.elapsed() >= Duration::from_secs(5) {
                break "Timed out while connecting to PipeWire".into();
            }
        };
        if let Some(ready) = startup.take() {
            let error = failure;
            let _ = ready.send(Err(error.clone()));
            return Err(error);
        }
        // Disconnect before sleeping, retaining process identities across server restarts.
        drop(connection);
        if !matches!(
            stop.recv_timeout(Duration::from_secs(1)),
            Err(mpsc::RecvTimeoutError::Timeout)
        ) || !sink.active()
        {
            return Ok(());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pulse_ownership_never_falls_back_to_the_server_process() {
        let client = Properties::from([("pipewire.sec.pid".into(), "10".into())]);
        assert_eq!(owner_pid(&Properties::new(), &client, true, true), None);
        let mut node = Properties::from([("application.process.id".into(), "10".into())]);
        assert_eq!(owner_pid(&node, &client, true, true), None);
        node.insert("application.process.id".into(), "20".into());
        assert_eq!(owner_pid(&node, &client, true, true), Some(20));
        assert_eq!(owner_pid(&node, &client, false, true), None);
        assert_eq!(
            owner_pid(&Properties::new(), &client, true, false),
            Some(10)
        );
    }

    #[test]
    fn channel_aliases_preserve_stereo_and_duplicate_mono() {
        assert!(channels_match("MONO", "FL"));
        assert!(channels_match("MONO", "FR"));
        assert!(channels_match("RC", "FC"));
        assert!(channels_match("AUX0", "FL"));
        assert!(!channels_match("FL", "FR"));
        assert!(!channels_match("MONO", "FC"));
    }

    #[test]
    fn downmix_preserves_stereo_and_clips_non_finite_and_loud_samples() {
        assert_eq!(pcm16(f32::NAN), 0);
        assert_eq!(pcm16(f32::INFINITY), 0);
        assert_eq!(pcm16(1.5), i16::MAX);
        assert_eq!(pcm16(-1.5), i16::MIN);
        let frame: Vec<_> = [0.25_f32, -0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
            .into_iter()
            .flat_map(f32::to_le_bytes)
            .collect();
        assert_eq!(
            downmix(&frame),
            [8192_i16.to_le_bytes(), (-16384_i16).to_le_bytes()].concat()
        );
        assert_eq!(downmix(&frame[..31]), Vec::<u8>::new());
        let center: Vec<_> = [0.0_f32, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0]
            .into_iter()
            .flat_map(f32::to_le_bytes)
            .collect();
        let mixed = downmix(&center);
        assert_eq!(&mixed[..2], &mixed[2..]);
    }
}
