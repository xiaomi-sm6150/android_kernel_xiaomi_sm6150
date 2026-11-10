// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

use core::mem::MaybeUninit;

use crate::{
    content::DmaBufContent,
    poll::{FileMonitor, ProcessMode},
    wrap::{Wrap, WRAP_FOPS},
};

use kernel::{
    bindings, device,
    fs::LocalFile,
    prelude::*,
    sync::{poll::PollTable, Arc},
    transmute::{AsBytes, FromBytes},
    types::ARef,
    uaccess::{UserSlice, UserSliceReader},
    uapi,
};

pub(crate) struct WrapFdDevice;

#[pin_data]
pub(crate) struct WrapfdDevFile {
    wrapping_dev: ARef<device::Device>,
    file_monitor: Arc<FileMonitor>,
}

#[vtable]
impl kernel::miscdevice::MiscDevice for WrapFdDevice {
    type File = Pin<KBox<WrapfdDevFile>>;

    fn open(
        _file: &kernel::fs::File,
        misc: &kernel::miscdevice::MiscDeviceRegistration<Self>,
    ) -> Result<Self::File> {
        let wrapping_dev = kernel::types::ARef::from(misc.device());

        KBox::try_pin_init(
            try_pin_init! {
                WrapfdDevFile { wrapping_dev,
                file_monitor: Arc::pin_init(FileMonitor::new(), GFP_KERNEL)? }
            },
            GFP_KERNEL,
        )
    }

    fn ioctl(
        me: Pin<&WrapfdDevFile>,
        _file: &kernel::fs::File,
        cmd: u32,
        arg: usize,
    ) -> Result<isize> {
        use kernel::ioctl::_IOC_SIZE;
        match cmd {
            uapi::WRAPFD_DEV_IOC_WRAP => {
                let user_slice = UserSlice::new(UserPtr::from_addr(arg), _IOC_SIZE(cmd));
                let wrap_args = user_slice.reader().read::<uapi::wrapfd_wrap>()?;

                Ok(WrapfdDevFile::wrap_file(&wrap_args)? as isize)
            }
            uapi::WRAPFD_DEV_IOC_ADD_MONITOR => {
                let user_slice = UserSlice::new(UserPtr::from_addr(arg), _IOC_SIZE(cmd));
                me.add_monitor(user_slice)
            }
            uapi::WRAPFD_DEV_IOC_DEL_MONITOR => {
                let user_slice = UserSlice::new(UserPtr::from_addr(arg), _IOC_SIZE(cmd));
                me.del_monitor(user_slice)
            }
            uapi::WRAPFD_DEV_IOC_QUERY_EVENTS => {
                let user_slice = UserSlice::new(UserPtr::from_addr(arg), _IOC_SIZE(cmd));
                me.query_events(user_slice)
            }
            uapi::WRAPFD_DEV_IOC_REPOPULATE => {
                let user_slice = UserSlice::new(UserPtr::from_addr(arg), _IOC_SIZE(cmd));
                let repop_args = user_slice.reader().read::<uapi::wrapfd_repopulate>()?;

                if repop_args.reserved != 0 {
                    return Err(EINVAL);
                }

                Ok(WrapfdDevFile::repopulate(&repop_args)? as isize)
            }
            _ => Err(ENOTTY),
        }
    }

    fn poll(
        device: Pin<&WrapfdDevFile>,
        file: &kernel::fs::File,
        table: PollTable<'_>,
    ) -> Result<bindings::__poll_t> {
        device.file_monitor.poll(file, table)
    }
}

impl WrapfdDevFile {
    fn wrap_file(args: &uapi::wrapfd_wrap) -> Result<u32> {
        if (args.prot & !(bindings::PROT_READ | bindings::PROT_WRITE)) != 0 {
            return Err(EINVAL);
        }

        if args.reserved != 0 {
            return Err(EINVAL);
        }

        let content = DmaBufContent::new(args.fd, args.prot)?;

        // SAFETY: `args.fd` is a valid file descriptor, otherwise `DmaBufContent::new` would have
        // failed.
        let close_on_exec = unsafe { bindings::get_close_on_exec(args.fd) };

        let wrap: Pin<KBox<Wrap>> = KBox::pin_init(Wrap::new(close_on_exec), GFP_KERNEL)?;

        DmaBufContent::publish_wrap(wrap, content, close_on_exec, false)
            .map_err(|(err, _content)| err)
    }

    fn add_monitor(&self, user_slice: UserSlice) -> Result<isize> {
        let mut reader = user_slice.reader();
        let args = reader.read::<uapi::wrapfd_add_monitors>()?;

        if args.reserved != 0 {
            return Err(EINVAL);
        }

        self.process_monitors(&args.monitors, ProcessMode::Add)
    }

    fn del_monitor(&self, user_slice: UserSlice) -> Result<isize> {
        let mut reader = user_slice.reader();
        let args = reader.read::<uapi::wrapfd_del_monitors>()?;

        if args.reserved != 0 {
            return Err(EINVAL);
        }

        self.process_monitors(&args.monitors, ProcessMode::Delete)
    }

    fn query_events(&self, user_slice: UserSlice) -> Result<isize> {
        let (mut reader, mut writer) = user_slice.reader_writer();
        let mut args = reader.read::<uapi::wrapfd_events>()?;

        if args.len == 0 || args.arr == 0 || args.len > (uapi::WRAPFD_MAX_NR_EVENTS as u64) {
            return Err(EINVAL);
        }

        let max_num_events: usize = args.len.try_into()?;

        let events_slice = UserSlice::new(
            UserPtr::from_addr(usize::try_from(args.arr)?),
            max_num_events * core::mem::size_of::<uapi::wrapfd_event>(),
        );

        self.file_monitor.query_events(max_num_events, |events| {
            let mut events_writer = events_slice.writer();
            events_writer.write_slice(events.as_bytes())?;

            args.count = events.len() as u64;
            writer.write(&args)
        })?;

        Ok(0)
    }

    /// Reads exactly `len` elements from `reader` into a newly allocated `KVec` and returns it
    /// (or an error).
    fn read_vec<T>(reader: &mut UserSliceReader, len: usize) -> Result<KVec<T>>
    where
        T: FromBytes,
    {
        let mut vec = KVec::<T>::with_capacity(len, GFP_KERNEL)?;

        let spare: &mut [MaybeUninit<T>] = &mut vec.spare_capacity_mut()[..len];
        let byte_len = core::mem::size_of_val(spare);

        // SAFETY:
        // - `spare` is valid for writes of `byte_len` bytes for its lifetime, and casting from
        //   `MaybeUninit<T>` to `MaybeUninit<u8>` has alignment 1 (always satisfied) with no
        //   initialization invariants.
        // - `byte_len` cannot exceed `isize::MAX` because `spare` is an existing Rust slice.
        let out_bytes: &mut [MaybeUninit<u8>] = unsafe {
            core::slice::from_raw_parts_mut(spare.as_mut_ptr().cast::<MaybeUninit<u8>>(), byte_len)
        };

        // Perform a single bulk copy from userspace; on `Ok(())`, `read_raw` guarantees every
        // byte in `out_bytes` has been initialized.
        reader.read_raw(out_bytes)?;

        // SAFETY:
        // - `with_capacity(len)` reserved at least `len` elements.
        // - `read_raw` succeeded, so all `byte_len` bytes covering `0..len` are initialized,
        //   and `T: FromBytes` guarantees any initialized bit pattern is a valid `T`.
        unsafe { vec.inc_len(len) };

        Ok(vec)
    }

    fn process_monitors(&self, args: &uapi::wrapfd_monitors, mode: ProcessMode) -> Result<isize> {
        if args.len == 0 || args.len > (uapi::WRAPFD_MAX_NR_MONITORS as u64) {
            return Err(EINVAL);
        }

        if args.arr == 0 || args.results == 0 {
            return Err(EFAULT);
        }

        let num_monitors: usize = args.len.try_into()?;

        let monitors_slice = UserSlice::new(
            UserPtr::from_addr(usize::try_from(args.arr)?),
            num_monitors * core::mem::size_of::<uapi::wrapfd_monitor>(),
        );
        let mut monitors_reader = monitors_slice.reader();
        let monitors = Self::read_vec(&mut monitors_reader, num_monitors)?;

        let results_slice = UserSlice::new(
            UserPtr::from_addr(usize::try_from(args.results)?),
            num_monitors * core::mem::size_of::<i64>(),
        );

        let mut results: KVec<i64> = KVec::with_capacity(num_monitors, GFP_KERNEL)?;
        for monitor in &monitors {
            // NOTE: Should never fail as we reserved the memory ahead of time above.
            results.push_within_capacity(
                FileMonitor::process_monitor(&self.file_monitor, monitor, mode)
                    .map_or_else(Error::to_errno, |_| 0)
                    .into(),
            )?;
        }

        let mut results_writer = results_slice.writer();
        results_writer.write_slice(results.as_bytes())?;

        Ok(0)
    }

    fn repopulate(args: &uapi::wrapfd_repopulate) -> Result<u32> {
        let local_file = LocalFile::fget(args.wrapfd)?;

        // SAFETY: There are no `fdget_pos` calls on the current thread.
        let file = unsafe { LocalFile::assume_no_fdget_pos(local_file) };

        let Some(wrap) = WRAP_FOPS.borrow_private(&*file) else {
            return Err(EBADF);
        };

        let close_on_exec = wrap.repopulate(&file, args.fd)?;

        // SAFETY: args.wrapfd was a valid file descriptor at least at the time we acquired the
        // `LocalFile` reference above. If the file descriptor has been invalidated since, the
        // call to `set_close_on_exec` will fail, but no corruption will occur because `wrapfd`
        // was in range of the ftable before and still is. The worst that can happen is that the
        // file descriptor has been reused and we set close_on_exec on a different file than we
        // intended.
        unsafe { bindings::set_close_on_exec(args.wrapfd, if close_on_exec { 1 } else { 0 }) };

        Ok(0)
    }
}
