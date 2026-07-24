//! Shared rustbus plumbing for the D-Bus-backed services (battery,
//! network, mpris).
//!
//! rustbus, not zbus, is the locked decision: pure-Rust, synchronous,
//! exposes the socket fd — after a blocking setup the connection
//! lives as a calloop `Generic` source and `refill_all()` drains it
//! nonblocking. No async executor, no threads, no idle wakeups.
//!
//! Conventions every consumer follows:
//! - one connection per service — match rules never interfere;
//! - match rules registered *before* the initial snapshot request, so
//!   a change racing the snapshot applies on top instead of being
//!   lost;
//! - setup (connect + AddMatch + initial state) is the only blocking
//!   IO, bounded by [`SETUP`];
//! - runtime requests go out with [`send`] and come back through
//!   `try_get_response` on the fd source — never a blocking wait.

use std::time::Duration;

use rustbus::connection::ll_conn::force_finish_on_error;
use rustbus::connection::Timeout;
use rustbus::message_builder::MarshalledMessage;
use rustbus::signature;
use rustbus::wire::unmarshal::UnmarshalContext;
use rustbus::{MessageBuilder, MessageType, RpcConn, Signature, Unmarshal};

pub(crate) const PROPS_IFACE: &str = "org.freedesktop.DBus.Properties";

/// Blocking-setup timeout (connect + AddMatch + initial snapshot).
pub(crate) const SETUP: Timeout = Timeout::Duration(Duration::from_secs(2));

#[derive(Debug, thiserror::Error)]
pub enum DbusError {
    #[error(transparent)]
    Bus(#[from] rustbus::connection::Error),
    #[error("marshal: {0}")]
    Marshal(#[from] rustbus::wire::errors::MarshalError),
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("error reply: {0}")]
    Reply(String),
    #[error("event loop: {0}")]
    Loop(String),
}

/// rustbus's typed layer has no `f64` (doubles only exist in its
/// dynamic `params` API as raw bits) — this newtype fills the gap.
pub(crate) struct Dbl(pub f64);

impl Signature for Dbl {
    fn signature() -> signature::Type {
        signature::Type::Base(signature::Base::Double)
    }
    fn alignment() -> usize {
        8
    }
}

impl<'buf, 'fds> Unmarshal<'buf, 'fds> for Dbl {
    fn unmarshal(
        ctx: &mut UnmarshalContext<'fds, 'buf>,
    ) -> rustbus::wire::unmarshal::UnmarshalResult<Self> {
        u64::unmarshal(ctx).map(|(bytes, bits)| (bytes, Dbl(f64::from_bits(bits))))
    }
}

/// Fail on a D-Bus error reply.
pub(crate) fn reply_ok(reply: &MarshalledMessage) -> Result<(), DbusError> {
    if reply.typ == MessageType::Error {
        return Err(DbusError::Reply(
            reply
                .dynheader
                .error_name
                .clone()
                .unwrap_or_else(|| "unknown D-Bus error".into()),
        ));
    }
    Ok(())
}

/// Send one message, flushing fully (messages are tiny; the socket's
/// send buffer never fills at the cadence a bar generates calls —
/// this cannot stall).
pub(crate) fn send(rpc: &mut RpcConn, msg: &mut MarshalledMessage) -> Result<u32, DbusError> {
    Ok(rpc
        .send_message(msg)?
        .write_all()
        .map_err(force_finish_on_error)?)
}

/// Build a `Properties.GetAll` call.
pub(crate) fn get_all(dest: &str, path: &str, iface: &str) -> Result<MarshalledMessage, DbusError> {
    let mut call = MessageBuilder::new()
        .call("GetAll")
        .with_interface(PROPS_IFACE)
        .on(path)
        .at(dest)
        .build();
    call.body.push_param(iface)?;
    Ok(call)
}

/// Build a `Properties.Get` call.
pub(crate) fn get_one(
    dest: &str,
    path: &str,
    iface: &str,
    prop: &str,
) -> Result<MarshalledMessage, DbusError> {
    let mut call = MessageBuilder::new()
        .call("Get")
        .with_interface(PROPS_IFACE)
        .on(path)
        .at(dest)
        .build();
    call.body.push_param(iface)?;
    call.body.push_param(prop)?;
    Ok(call)
}

#[cfg(test)]
pub(crate) mod test_util {
    //! Build real signal messages through rustbus's dynamic
    //! marshaller, so model tests parse actual wire bytes through the
    //! same typed path used live.

    use rustbus::message_builder::MarshalledMessage;
    use rustbus::params::{Container, Param, Variant as ParamVariant};
    use rustbus::MessageBuilder;

    use super::PROPS_IFACE;

    /// A `PropertiesChanged` signal (`s a{sv} as`) for `iface` at
    /// `path`.
    pub(crate) fn props_changed(
        path: &str,
        iface: &str,
        props: Vec<(&str, Param<'static, 'static>)>,
    ) -> MarshalledMessage {
        let mut msg = MessageBuilder::new()
            .signal(PROPS_IFACE, "PropertiesChanged", path)
            .build();
        msg.body.push_param(iface).unwrap();
        msg.body
            .push_old_param(&Param::Container(dict(props)))
            .unwrap();
        msg.body.push_param::<&[&str]>(&[]).unwrap(); // invalidated props
        msg
    }

    /// An `a{sv}` container from `(key, value)` pairs.
    pub(crate) fn dict(props: Vec<(&str, Param<'static, 'static>)>) -> Container<'static, 'static> {
        Container::make_dict(
            "s",
            "v",
            props.into_iter().map(|(k, v)| {
                (
                    k.to_string(),
                    Param::Container(Container::Variant(Box::new(ParamVariant {
                        sig: v.sig(),
                        value: v,
                    }))),
                )
            }),
        )
        .unwrap()
    }
}
