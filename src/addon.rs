use crate::capture::{lock, AudioSink, CaptureMode, Mailbox, Session};
use napi::{
    bindgen_prelude::{Buffer, Function},
    threadsafe_function::{ErrorStrategy, ThreadsafeFunction, ThreadsafeFunctionCallMode},
    CallContext, Env, Error, JsFunction, JsNumber, JsObject, JsUndefined, JsUnknown, Property,
    Result, Status, ValueType,
};
use napi_derive::{js_function, module_exports};
use std::sync::{Arc, Mutex, Weak};

type Registry = Arc<Mutex<Vec<Weak<Session>>>>;

// One cleanup hook per Node environment, including Worker environments. It runs
// before Node disposes its callback handles, and never invokes JavaScript.
fn registry(env: &mut Env) -> Result<Registry> {
    if let Some(registry) = env.get_instance_data::<Registry>()? {
        return Ok(registry.clone());
    }
    let registry: Registry = Arc::default();
    env.set_instance_data(registry.clone(), (), |_| {})?;
    env.add_env_cleanup_hook(registry.clone(), |registry| {
        let sessions: Vec<_> = lock(&registry).iter().filter_map(Weak::upgrade).collect();
        for session in sessions {
            let _ = session.stop();
        }
    })?;
    Ok(registry)
}

#[derive(Default)]
struct Capture {
    session: Option<Arc<Session>>,
}

impl Capture {
    fn start(
        &mut self,
        mut env: Env,
        mode: CaptureMode,
        callback: napi::bindgen_prelude::FunctionRef<(Buffer,), JsUnknown>,
    ) -> Result<()> {
        if self.session.is_some() {
            return Err(Error::from_reason(
                "Capture already started on this instance",
            ));
        }
        let registry = registry(&mut env)?;
        let mailbox = Mailbox::new();
        let delivery = mailbox.clone();
        // The dispatcher and user callback exist only on the JS thread. Workers
        // pass owned bytes through a mailbox, never JsValues or borrowed buffers.
        let dispatcher = env.create_function_from_closure("deliverAudio", move |ctx| {
            let packets = delivery.drain();
            for packet in packets {
                if !delivery.active() {
                    break;
                }
                // A callback may stop/restart capture. Never hold a lock or a
                // mutable reference to Capture across this reentrant JS call.
                callback
                    .borrow_back(ctx.env)?
                    .call((Buffer::from(packet),))?;
            }
            ctx.env.get_undefined()
        })?;
        // Audio is bounded in the mailbox, not in napi-rs' TSFN. Coalescing means
        // at most one wakeup is queued, regardless of how far JS falls behind.
        let tsfn: ThreadsafeFunction<(), ErrorStrategy::Fatal> =
            dispatcher.create_threadsafe_function(0, |_ctx| Ok(Vec::<JsUnknown>::new()))?;
        let sink = AudioSink::new(mailbox, move || {
            tsfn.call((), ThreadsafeFunctionCallMode::NonBlocking) == Status::Ok
        });
        let session = Session::start(mode, sink).map_err(Error::from_reason)?;
        let mut sessions = lock(&registry);
        sessions.retain(|session| session.strong_count() > 0);
        sessions.push(Arc::downgrade(&session));
        self.session = Some(session);
        Ok(())
    }

    fn stop(&mut self) -> Result<()> {
        if let Some(session) = self.session.take() {
            session.stop().map_err(Error::from_reason)?;
        }
        Ok(())
    }
}

impl Drop for Capture {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

fn invalid_type(env: &Env, message: &str) -> Result<JsUndefined> {
    env.throw_type_error(message, None)?;
    env.get_undefined()
}

fn process_id(env: &Env, value: JsUnknown, name: &str) -> Result<Option<u32>> {
    if value.get_type()? != ValueType::Number {
        env.throw_type_error(&format!("{name} must be a number"), None)?;
        return Ok(None);
    }
    let number = JsNumber::try_from(value)?.get_double()?;
    let maximum = if cfg!(target_os = "windows") {
        u32::MAX
    } else {
        i32::MAX as u32
    };
    if !number.is_finite() || number < 1.0 || number > maximum as f64 || number.fract() != 0.0 {
        env.throw_range_error(
            &format!("{name} must be an integer between 1 and {maximum}"),
            None,
        )?;
        return Ok(None);
    }
    Ok(Some(number as u32))
}

#[js_function(1)]
fn constructor(ctx: CallContext) -> Result<JsUndefined> {
    let mut this: JsObject = ctx.this()?;
    ctx.env.wrap(&mut this, Capture::default())?;
    ctx.env.get_undefined()
}

#[js_function(3)]
fn start(ctx: CallContext) -> Result<JsUndefined> {
    if ctx.length != 2 || ctx.get::<JsUnknown>(1)?.get_type()? != ValueType::Function {
        return invalid_type(
            ctx.env,
            "Expected start(processId: number, onData: (chunk: Buffer) => void)",
        );
    }
    let Some(pid) = process_id(ctx.env, ctx.get(0)?, "processId")? else {
        return ctx.env.get_undefined();
    };
    let callback = ctx.get::<Function<(Buffer,), JsUnknown>>(1)?.create_ref()?;
    let this: JsObject = ctx.this()?;
    ctx.env
        .unwrap::<Capture>(&this)?
        .start(*ctx.env, CaptureMode::Include(pid), callback)?;
    ctx.env.get_undefined()
}

#[js_function(3)]
fn start_system_audio(ctx: CallContext) -> Result<JsUndefined> {
    if ctx.length < 1
        || ctx.length > 2
        || ctx.get::<JsUnknown>(0)?.get_type()? != ValueType::Function
    {
        return invalid_type(
            ctx.env,
            "Expected startSystemAudio(onData: (chunk: Buffer) => void, excludeProcessId?: number)",
        );
    }
    let mode = if ctx.length == 2 && ctx.get::<JsUnknown>(1)?.get_type()? != ValueType::Undefined {
        let Some(pid) = process_id(ctx.env, ctx.get(1)?, "excludeProcessId")? else {
            return ctx.env.get_undefined();
        };
        CaptureMode::Exclude(pid)
    } else {
        CaptureMode::System
    };
    let callback = ctx.get::<Function<(Buffer,), JsUnknown>>(0)?.create_ref()?;
    let this: JsObject = ctx.this()?;
    ctx.env
        .unwrap::<Capture>(&this)?
        .start(*ctx.env, mode, callback)?;
    ctx.env.get_undefined()
}

#[js_function(1)]
fn stop(ctx: CallContext) -> Result<JsUndefined> {
    let this: JsObject = ctx.this()?;
    ctx.env.unwrap::<Capture>(&this)?.stop()?;
    ctx.env.get_undefined()
}

#[js_function(1)]
fn last_error(ctx: CallContext) -> Result<Option<String>> {
    let this: JsObject = ctx.this()?;
    Ok(ctx
        .env
        .unwrap::<Capture>(&this)?
        .session
        .as_ref()
        .and_then(|session| session.error()))
}

#[module_exports]
fn init(mut exports: JsObject, env: Env) -> Result<()> {
    let constructor: JsFunction = env.define_class(
        "LoopbackCapture",
        constructor,
        &[
            Property::new("start")?.with_method(start),
            Property::new("startSystemAudio")?.with_method(start_system_audio),
            Property::new("stop")?.with_method(stop),
            Property::new("lastError")?.with_getter(last_error),
        ],
    )?;
    exports.set_named_property("LoopbackCapture", constructor)?;
    Ok(())
}
