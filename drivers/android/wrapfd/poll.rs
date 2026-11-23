// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

use crate::wrap::WRAP_FOPS;

use kernel::{
    bindings,
    fs::LocalFile,
    new_mutex, new_poll_condvar,
    prelude::*,
    sync::{
        atomic::{
            ordering::{Acquire, Release},
            Atomic,
        },
        poll::{PollCondVar, PollTable},
        Arc, Mutex,
    },
    uapi,
};

#[derive(Default)]
struct EventInfo {
    monitored: u32,
    triggered: u32,
    timestamp: u64,
    seq_num: i64,
}

impl EventInfo {
    fn new(seq_num: i64) -> Self {
        Self {
            monitored: 0,
            triggered: 0,
            timestamp: 0,
            seq_num,
        }
    }
}

type EventVector = KVec<Option<EventInfo>>;

#[derive(Default)]
struct FileMonitorInner {
    events: EventVector,
    next_seq_num: i64,
}

#[pin_data]
pub(crate) struct FileMonitor {
    #[pin]
    condvar: PollCondVar,
    events_pending: Atomic<bool>,

    #[pin]
    inner: Mutex<FileMonitorInner>,
}

struct SlotInfo {
    slot: usize,
    seq_num: i64,
}

impl FileMonitor {
    pub(crate) fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            condvar <- new_poll_condvar!("FileMonitor::condvar"),
            events_pending: Atomic::new(false),
            inner <- new_mutex!(FileMonitorInner::default()),
        })
    }

    pub(crate) fn clear_all_slots(&self) {
        let mut guard = self.inner.lock();
        for info in &mut guard.events {
            *info = None;
        }
    }

    fn allocate_slot(&self, inner: &mut FileMonitorInner) -> Result<(usize, i64)> {
        let seq_num = inner.next_seq_num;

        // Make sure we wrap around to 0 if otherwise this would turn into a negative i64.
        inner.next_seq_num = inner.next_seq_num.checked_add(1).unwrap_or(0);

        let info = EventInfo::new(seq_num);

        if let Some(slot) = inner.events.iter().position(|entry| entry.is_none()) {
            inner.events[slot] = Some(info);
            return Ok((slot, seq_num));
        }

        let slot = inner.events.len();

        // If this fails we'll have already incremented `self.next_seq_num` but that's not
        // problematic.
        inner.events.push(Some(info), GFP_KERNEL)?;

        Ok((slot, seq_num))
    }

    pub(crate) fn free_slot(&self, slot: usize, seq_num: i64) -> Result<()> {
        let mut guard = self.inner.lock();
        if slot >= guard.events.len() {
            return Err(ERANGE);
        }
        let Some(info) = &mut guard.events[slot] else {
            return Ok(());
        };
        if info.seq_num == seq_num {
            guard.events[slot] = None;
        }

        Ok(())
    }

    pub(crate) fn generate_wrap_event(&self, slot: usize, seq_num: i64, event: u32) -> Result<()> {
        let should_wake = {
            let mut guard = self.inner.lock();
            if slot >= guard.events.len() {
                return Err(ERANGE);
            }

            let Some(info) = &mut guard.events[slot] else {
                return Err(EINVAL);
            };

            if info.seq_num != seq_num {
                return Err(EINVAL);
            }

            if (info.monitored & event) == 0 {
                return Ok(());
            }

            info.triggered |= event;

            // SAFETY: Calling `ktime_get_ns` is always safe.
            info.timestamp = unsafe { bindings::ktime_get_ns() };

            !self.events_pending.xchg(true, Release)
        };

        if should_wake {
            self.condvar.notify_all();
        }

        Ok(())
    }

    pub(crate) fn add_monitor(filemon: &Arc<Self>, args: &uapi::wrapfd_add_monitor) -> Result<i64> {
        if (args.events & !uapi::WRAPFD_EVENTS_MASK) != 0
            || (args.events & uapi::WRAPFD_EVENTS_MASK) == 0
            || args.pad != 0
        {
            return Err(EINVAL);
        }

        let Ok(local_file) = LocalFile::fget(args.fd) else {
            return Err(EBADF);
        };

        let Some(wrap) = WRAP_FOPS.borrow_private(&*local_file) else {
            return Err(EBADF);
        };

        let mut wrap_guard = wrap.inner.lock();
        let mut filemon_guard = filemon.inner.lock();
        if let Some((slot, seq_num)) = wrap_guard.get_slot(filemon) {
            if let Some(ref mut info) = &mut filemon_guard.events[slot] {
                if info.seq_num == seq_num {
                    info.monitored |= args.events;
                    return Ok(seq_num);
                }
            }

            // The wrap's previously assigned slot no longer belongs to it, it's either been cleared
            // altogether or it's been reused since.
            _ = wrap_guard.detach_monitor(filemon);
        }

        let (slot, seq_num) = filemon.allocate_slot(&mut filemon_guard)?;
        wrap_guard
            .attach_monitor(Arc::clone(filemon), slot, seq_num)
            .inspect_err(|_err| {
                filemon_guard.events[slot] = None;
            })?;
        filemon_guard.events[slot].as_mut().unwrap().monitored |= args.events;

        Ok(seq_num)
    }

    pub(crate) fn delete_monitor(
        filemon: &Arc<Self>,
        args: &uapi::wrapfd_del_monitor,
    ) -> Result<()> {
        if (args.events & !uapi::WRAPFD_EVENTS_MASK) != 0
            || (args.events & uapi::WRAPFD_EVENTS_MASK) == 0
            || args.pad != 0
        {
            return Err(EINVAL);
        }

        // `id` is really a `u63`, or rather a non-negative `i64`.
        if (args.id & (1 << 63)) != 0 {
            return Err(EINVAL);
        }

        let mut filemon_guard = filemon.inner.lock();
        let Some((slot, info)) =
            filemon_guard
                .events
                .iter_mut()
                .enumerate()
                .find_map(|(slot, info)| {
                    let info = info.as_mut()?;
                    (info.seq_num == (args.id as i64)).then_some((slot, info))
                })
        else {
            return Err(EINVAL);
        };

        info.monitored &= !args.events;
        info.triggered &= info.monitored;
        if info.monitored == 0 {
            // Cleanup inside the wrap happens lazily so we do not need to acquire the wrap's
            // lock here. We will eventually discover that the wrap's slot has been invalidated
            // (as a result of calling `generate_wrap_event` or while processing `add_monitor`)
            // and clean up the wrap-internal list of monitor entries.
            filemon_guard.events[slot] = None;
        }

        Ok(())
    }

    pub(crate) fn query_events<F>(&self, max_num_events: usize, f: F) -> Result<()>
    where
        F: FnOnce(&[uapi::wrapfd_event]) -> Result,
    {
        let mut events = KVec::<uapi::wrapfd_event>::with_capacity(max_num_events, GFP_KERNEL)?;
        let mut slot_infos = KVec::<SlotInfo>::with_capacity(max_num_events, GFP_KERNEL)?;

        {
            let mut filemon_guard = self.inner.lock();

            let mut has_more = false;
            for (slot, opt_info) in filemon_guard.events.iter_mut().enumerate() {
                let Some(info) = opt_info else {
                    continue;
                };

                if info.triggered == 0 {
                    continue;
                }

                let Ok(()) = slot_infos.push_within_capacity(SlotInfo {
                    slot,
                    seq_num: info.seq_num,
                }) else {
                    has_more = true;
                    break;
                };

                // If our previous `push_within_capacity` succeeded, so will this one, both vectors
                // have the same capacity.
                _ = events.push_within_capacity(uapi::wrapfd_event {
                    id: info.seq_num as u64,
                    events: info.triggered,
                    timestamp: info.timestamp,
                    pad: 0,
                });

                info.triggered = 0;
            }

            if !has_more {
                self.events_pending.store(false, Release);
            }
        }

        if let Err(err) = f(&events) {
            if events.is_empty() {
                // There's nothing for us to restore, so no need for wakeup either.
                return Err(err);
            }

            let should_wake = {
                let mut filemon_guard = self.inner.lock();

                let mut restored_any = false;
                for (SlotInfo { slot, seq_num }, ev) in slot_infos.into_iter().zip(events.iter()) {
                    let Some(Some(info)) = filemon_guard.events.get_mut(slot) else {
                        continue;
                    };

                    if info.seq_num != seq_num {
                        // This slot has since been reused, there's nothing for us to restore here.
                        continue;
                    }

                    // Make sure that we only restore bits that are still monitored now, after all
                    // we briefly released our lock, so a `delete_monitor` may have been issued.
                    let restore_mask = ev.events & info.monitored;

                    if restore_mask == 0 {
                        continue;
                    }

                    // It wouldn't do to just assign `ev.events` back to `info.triggered`, since we
                    // released the lock albeit briefly, other events may have been or'ed into the
                    // `triggered` mask already.
                    info.triggered |= restore_mask;
                    restored_any = true;
                }

                restored_any && !self.events_pending.xchg(true, Release)
            };

            if should_wake {
                self.condvar.notify_all();
            }

            return Err(err);
        }

        Ok(())
    }

    pub(crate) fn poll(
        &self,
        file: &kernel::fs::File,
        table: PollTable<'_>,
    ) -> Result<bindings::__poll_t> {
        table.register_wait(file, &self.condvar);

        let mut mask = 0;
        if self.events_pending.load(Acquire) {
            mask |= bindings::POLLIN | bindings::POLLRDNORM;
        }

        Ok(mask)
    }
}

impl PartialEq for FileMonitor {
    fn eq(&self, other: &Self) -> bool {
        core::ptr::eq(self, other)
    }
}

impl Eq for FileMonitor {}
