// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

use crate::io_context::IoContext;
use crate::wrap::{create_wrap_file, Wrap};

use kernel::{
    bindings, c_str,
    dma_buf::{CpuAccess, DmaBuf, DmaBufVmap},
    ffi,
    fs::{file::mode, file::FileDescriptorReservation, file::Offset, LocalFile},
    new_condvar, new_mutex,
    prelude::*,
    sync::{Arc, CondVar, Mutex},
    types::ARef,
};

pub(crate) struct Listener {
    id: u64,
    ops: *const bindings::wrapfd_listener_operations,
    param: *mut ffi::c_void,
}

// SAFETY: `Listener` can be safely sent across threads.
unsafe impl Send for Listener {}

// SAFETY: `Listener` can be shared across threads.
unsafe impl Sync for Listener {}

enum ContentListenersState {
    Idle,
    NotifyingListener(bindings::pid_t, u64),
}

struct ContentListenersInner {
    next_listener_id: u64,
    listeners: KVec<Pin<KBox<Listener>>>,
    state: ContentListenersState,
}

#[pin_data]
pub(crate) struct ContentListeners {
    #[pin]
    inner: Mutex<ContentListenersInner>,

    #[pin]
    condvar: CondVar,
}

impl ContentListeners {
    fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            inner <- new_mutex!(ContentListenersInner {
                next_listener_id: 0,
                listeners: KVec::default(),
                state: ContentListenersState::Idle,
            }),
            condvar <- new_condvar!(),
        })
    }
}

pub(crate) struct DmaBufContent {
    pub(crate) dmabuf: ARef<DmaBuf>,
    writable: bool,

    pub(crate) listeners: Arc<ContentListeners>,
}

impl DmaBufContent {
    fn is_dmabuf_writable(dmabuf: &DmaBuf) -> bool {
        (dmabuf.file().mode() & mode::FMODE_WRITE) != 0
    }

    pub(crate) fn make_writable(&mut self, writable: bool) -> Result<()> {
        if writable && !Self::is_dmabuf_writable(&self.dmabuf) {
            return Err(EACCES);
        }

        self.writable = writable;

        Ok(())
    }

    fn with_content_mapped_for_writing<F>(&mut self, f: F) -> Result
    where
        F: FnOnce(&mut Self, CpuAccess<'_, false, true>) -> Result,
    {
        let map = self.dmabuf.vmap()?;
        map.begin_cpu_access_to_fallible(|access| f(self, access))
    }

    pub(crate) fn is_writable(&self) -> bool {
        self.writable && Self::is_dmabuf_writable(&self.dmabuf)
    }

    pub(crate) fn new(fd: u32, prot: u32) -> Result<Self> {
        // Userspace relies on this error to be EBADF if the u32 were to overflow an i32.
        // `i32::try_from()` results in `TryFromIntError` in that case, which `Error`
        // converts to EINVAL by default.
        let fd = i32::try_from(fd).map_err(|_| EBADF)?;
        let dmabuf = DmaBuf::get(fd)?;
        let writable = (prot & bindings::PROT_WRITE) != 0;

        if writable && !Self::is_dmabuf_writable(&dmabuf) {
            return Err(EACCES);
        }

        Ok(Self {
            dmabuf,
            writable,
            listeners: Arc::pin_init(ContentListeners::new(), GFP_KERNEL)?,
        })
    }

    pub(crate) fn publish_wrap(
        wrap: Pin<KBox<Wrap>>,
        mut content: DmaBufContent,
        close_on_exec: bool,
        clear_content: bool,
    ) -> Result<u32, (Error, DmaBufContent)> {
        let size = content.dmabuf.size();
        let size = match Offset::try_from(size) {
            Err(_err) => {
                return Err((EINVAL, content));
            }
            Ok(size) => size,
        };

        let writable = content.is_writable();

        let mut flags = 0;
        if close_on_exec {
            flags |= bindings::O_CLOEXEC
        };
        if writable {
            flags |= bindings::O_RDWR
        } else {
            flags |= bindings::O_RDONLY
        };

        let reservation = match FileDescriptorReservation::get_unused_fd_flags(flags) {
            Err(err) => {
                return Err((err, content));
            }
            Ok(reservation) => reservation,
        };

        let inner_clone = Arc::clone(&wrap.inner);

        let file = match create_wrap_file(c_str!("[wrapfd]"), wrap, flags) {
            Ok(file) => file,
            Err((err, _wrap)) => return Err((err, content)),
        };

        if clear_content {
            let mut closure_ran = false;
            if let Err(err) = content.with_content_mapped_for_writing(|content, mut access| {
                content.listeners.notify_listeners();
                access.memset(0, 0, content.dmabuf.size());
                closure_ran = true;
                Ok(())
            }) {
                if !closure_ran {
                    // If we already ran the closure, i.e. the underlying `begin_cpu_access` was
                    // successful, this error resulted from `end_cpu_access` and we'll ignore it -
                    // this is consistent with the c-driver's approach.
                    return Err((err, content));
                }
            }
        }

        inner_clone.lock().content = Some(content);

        // SAFETY: `file` is a valid `kernel::fs::File` reference, ensuring `file.as_ptr()` is
        // non-null and points to an active `struct file`.
        let inode = unsafe { bindings::file_inode(file.as_ptr()) };

        // SAFETY: `file_inode` on a valid `struct file` returns a non-null, valid pointer to its
        // `struct inode`.
        unsafe { bindings::i_size_write(inode, size) };

        let fd = reservation.reserved_fd();
        reservation.fd_install(file);

        Ok(fd)
    }

    pub(crate) fn ioctl(dmabuf: &DmaBuf, cmd: u32, arg: usize) -> Result<isize> {
        let file = dmabuf.file();
        let file_ptr = file.as_ptr();

        // SAFETY: "file" is valid as "dmabuf" holds a reference to it.
        let f_op = unsafe { (*file_ptr).f_op };

        // SAFETY: `in_compat_syscall` is safe to call.
        let opt_ioctl_cb = if unsafe { bindings::in_compat_syscall() } {
            // SAFETY: `f_op` is a valid pointer to the file's `struct file_operations`.
            unsafe { (*f_op).compat_ioctl }
        } else {
            // SAFETY: `f_op` is a valid pointer to the file's `struct file_operations`.
            unsafe { (*f_op).unlocked_ioctl }
        };

        // SAFETY: `f_op` is a valid pointer to the file's `struct file_operations`.
        let Some(ioctl_cb) = opt_ioctl_cb else {
            return Err(ENOIOCTLCMD);
        };

        // SAFETY: `file_ptr` is valid, `unlocked_ioctl` is a verified non-null function pointer,
        // and `cmd`/`arg` are primitive values passed directly per C ABI.
        let res = unsafe { ioctl_cb(file_ptr, cmd, arg) };

        if res < 0 {
            Err(Error::from_errno(res as i32))
        } else {
            Ok(res)
        }
    }

    pub(crate) fn llseek(dmabuf: &DmaBuf, offs: Offset, whence: ffi::c_int) -> Result<Offset> {
        let file = dmabuf.file();
        let file_ptr = file.as_ptr();

        // SAFETY: "file" is valid as "dmabuf" holds a reference to it.
        let f_op = unsafe { (*file_ptr).f_op };

        // SAFETY: `f_op` is a valid pointer to the file's `struct file_operations`.
        let Some(llseek) = (unsafe { (*f_op).llseek }) else {
            return Err(ESPIPE);
        };

        // SAFETY: `file_ptr` is valid, `llseek` is a verified non-null function pointer,
        // and `cmd`/`arg` are primitive values passed directly per C ABI.
        let res = unsafe { llseek(file_ptr, offs, whence) };

        if res < 0 {
            Err(Error::from_errno(res as i32)) // XXX fishy
        } else {
            Ok(res)
        }
    }

    fn load_prepare(
        dmabuf: &DmaBuf,
        file: &LocalFile,
        file_offs: usize,
        buf_offs: usize,
        len: usize,
    ) -> Result<(DmaBufVmap, IoContext)> {
        /* We will only write into buf_offs + len, so no need to page-align the length here. */
        let Some(buf_end) = buf_offs.checked_add(len) else {
            return Err(EINVAL);
        };

        if buf_end > dmabuf.size() {
            return Err(EINVAL);
        }

        let map = dmabuf.vmap()?;

        let Some(vaddr) = map.vaddr() else {
            return Err(EINVAL);
        };

        // SAFETY: We've verified that the `dmabuf` has the required dimensions and `map` keeps
        // the buffer mapped across `read()`.
        let mut io_context = unsafe { IoContext::new(file_offs, vaddr, buf_offs, len)? };
        io_context.prepare(file)?;

        Ok((map, io_context))
    }

    pub(crate) fn load(
        dmabuf: &DmaBuf,
        file: ARef<LocalFile>,
        file_offs: Offset,
        buf_offs: Offset,
        len: Offset,
    ) -> Result<isize> {
        let file_offs: usize = file_offs.try_into()?;
        let buf_offs: usize = buf_offs.try_into()?;
        let len: usize = len.try_into()?;

        let (map, mut io_context) = Self::load_prepare(dmabuf, &file, file_offs, buf_offs, len)?;

        map.begin_cpu_access_bidirectional_fallible(|access| -> Result<()> {
            io_context.read()?;
            io_context.complete(access)?;

            Ok(())
        })?;

        Ok(0)
    }
}

impl ContentListeners {
    pub(crate) fn add_listener(
        &self,
        ops: *const bindings::wrapfd_listener_operations,
        param: *mut ffi::c_void,
    ) -> Result<u64> {
        let mut guard = self.inner.lock();

        if !matches!(guard.state, ContentListenersState::Idle) {
            // This should never happen because the content has been detached from any wrap and
            // therefore `wrapfd_add_listener` would fail early and never call through to this
            // method.
            return Err(EINVAL);
        }

        let id = guard.next_listener_id;
        guard.next_listener_id = guard.next_listener_id.wrapping_add(1);

        let listener = Box::pin(Listener { id, ops, param }, GFP_KERNEL)?;

        guard.listeners.push(listener, GFP_KERNEL)?;

        Ok(id)
    }

    fn remove_listener_internal(inner: &mut ContentListenersInner, id: u64) -> Result {
        let index = inner
            .listeners
            .iter()
            .position(|listener| listener.id == id)
            .ok_or(EINVAL)?;

        _ = inner.listeners.remove(index);

        Ok(())
    }

    pub(crate) fn remove_listener(&self, id: u64) -> Result {
        let mut guard = self.inner.lock();

        match guard.state {
            ContentListenersState::Idle => {
                _ = Self::remove_listener_internal(&mut guard, id);

                // We are successful either way, if we do find the listener, we remove it as there
                // is no contention for it. If we cannot find the listener and we know the `id` was
                // a valid one (`wrapfd_del_listener` will only call us with a valid `id`), we can
                // conclude that the listener was removed as part of the notification process.
                Ok(())
            }
            ContentListenersState::NotifyingListener(notify_task_pid, listener_id) => {
                if current!().pid() == notify_task_pid {
                    if listener_id == id {
                        // We are being called from within the `on_content_release` callback, the
                        // particulars of our `notify_listeners` implementation guarantee that we
                        // won't touch `ops` or `param` again, so it's safe to free them.
                        Ok(())
                    } else {
                        // Calls from within `on_content_release` may only remove the listener
                        // associated with that callback.
                        Err(EINVAL)
                    }
                } else if Self::remove_listener_internal(&mut guard, id).is_ok() {
                    // Since we're not on the same thread as `notify_listeners` and we were able to
                    // remove the listener from the vector it means that `notify_listeners` never
                    // got a chance to process it and we're done.
                    Ok(())
                } else {
                    // This listener has already been removed from the vector, it has either been
                    // fully processed or is in the process of being notified, we cannot guarantee
                    // that the thread executing `notify_listeners` has finished accessing `ops`
                    // and `param` until the state indicates that it's either processing another
                    // listener than the one we're interested in or is done processing altogether.
                    loop {
                        // TODO: Fix this monstrosity once we have support for let-chains.
                        if let ContentListenersState::NotifyingListener(_, active_id) = guard.state
                        {
                            if active_id != id {
                                // `notify_listeners` has moved on to another listener.
                                break;
                            }
                        } else {
                            // `notify_listeners` is done with all listeners.
                            break;
                        }

                        self.condvar.wait(&mut guard);
                    }

                    Ok(())
                }
            }
        }
    }

    pub(crate) fn notify_listeners(&self) {
        let current_thread_pid = current!().pid();

        loop {
            let listener = {
                let mut guard = self.inner.lock();
                let Some(listener) = guard.listeners.pop() else {
                    break;
                };
                guard.state =
                    ContentListenersState::NotifyingListener(current_thread_pid, listener.id);

                self.condvar.notify_all();

                listener
            };

            // SAFETY: We've previously verified that `ops` is a non-null pointer, the contract for
            // `wrapfd_add_listener` requires `ops` to point to a valid `struct
            // wrapfd_listener_operation` and remain valid for as long as the listener is
            // registered.
            let Some(on_content_release) = (unsafe { (*listener.ops).on_content_release }) else {
                continue;
            };
            // SAFETY: Safe by contract. Note that `listener` is dropped after calling the callback,
            // we therefore won't access `ops` or `param` again and it's safe to free them from
            // inside the callback.
            unsafe { on_content_release(listener.param) };
        }

        let mut guard = self.inner.lock();
        guard.state = ContentListenersState::Idle;
        self.condvar.notify_all();
    }
}

impl Drop for DmaBufContent {
    fn drop(&mut self) {
        self.listeners.notify_listeners();
    }
}
