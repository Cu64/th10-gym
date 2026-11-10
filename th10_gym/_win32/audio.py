"""Best-effort per-process Core Audio session mute for the owned TH10 PID."""

from __future__ import annotations

import ctypes
import os
import threading
import uuid
from contextlib import ExitStack, contextmanager
from ctypes import wintypes


class ProcessAudioMute:
    """Mute sessions created by one PID. Enumeration runs outside game updates."""

    def __init__(self, pid: int, *, interval: float = 0.5):
        if os.name != "nt":
            raise OSError("Per-process audio mute requires Windows Core Audio")
        self.pid = int(pid)
        self._interval = interval
        self._enabled = True
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._lock = threading.Lock()
        self._status = {"enabled": True, "pid": self.pid, "muted_sessions": 0, "error": None}
        self._thread = threading.Thread(target=self._run, name=f"th10-audio-{pid}", daemon=True)
        self._thread.start()

    @property
    def status(self):
        with self._lock:
            return dict(self._status)

    def set_muted(self, muted: bool):
        with self._lock:
            self._enabled = bool(muted)
        self._wake.set()

    def _run(self):
        while not self._stop.is_set():
            self._wake.clear()
            if self._stop.is_set():
                break
            with self._lock:
                enabled = self._enabled
            count = 0
            error = None
            try:
                count = _enumerate_and_set(self.pid, enabled)
            except Exception as exc:  # Keep the Gym process usable when audio APIs fail.
                error = f"{type(exc).__name__}: {exc}"
            with self._lock:
                self._status.update(enabled=enabled,
                                    muted_sessions=count if enabled else 0, error=error)
            self._wake.wait(self._interval if enabled else None)

    def close(self):
        self._stop.set()
        self._wake.set()
        self._thread.join(timeout=2)


class _GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_uint32), ("Data2", ctypes.c_uint16),
                ("Data3", ctypes.c_uint16), ("Data4", ctypes.c_ubyte * 8)]


def _guid(value):
    return _GUID.from_buffer_copy(uuid.UUID(value).bytes_le)


def _call(obj, index, restype, *argtypes):
    table = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    prototype = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)
    return prototype(table[index])


def _check(hr, operation):
    if hr < 0:
        raise OSError(f"{operation} failed with HRESULT 0x{hr & 0xFFFFFFFF:08X}")



_CLSID_DEVICE_ENUMERATOR = _guid("BCDE0395-E52F-467C-8E3D-C4579291692E")
_IID_DEVICE_ENUMERATOR = _guid("A95664D2-9614-4F35-A746-DE8DB63617E6")
_IID_SESSION_MANAGER = _guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")
_IID_SESSION_CONTROL = _guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")
_IID_SIMPLE_VOLUME = _guid("87CE5498-68D6-44E5-9215-6DA47EF883D8")
_CLSCTX_ALL = 23
_COINIT_APARTMENTTHREADED = 2
_RENDER, _CONSOLE = 0, 0
_POINTER = ctypes.c_void_p
_GUID_REF = ctypes.POINTER(_GUID)


@contextmanager
def _com_apartment():
    ole = ctypes.WinDLL("ole32")
    ole.CoInitializeEx.argtypes = (_POINTER, wintypes.DWORD)
    ole.CoInitializeEx.restype = ctypes.c_long
    ole.CoUninitialize.argtypes = ()
    ole.CoUninitialize.restype = None
    ole.CoCreateInstance.argtypes = (_GUID_REF, _POINTER, wintypes.DWORD, _GUID_REF,
                                    ctypes.POINTER(_POINTER))
    ole.CoCreateInstance.restype = ctypes.c_long
    _check(ole.CoInitializeEx(None, _COINIT_APARTMENTTHREADED), "CoInitializeEx")
    try:
        yield ole
    finally:
        ole.CoUninitialize()


def _acquire(scope, operation, call, *args):
    """Own the interface reference returned by a successful COM call."""
    result = _POINTER()
    _check(call(*args, ctypes.byref(result)), operation)
    if not result:
        raise OSError(f"{operation} returned no interface")
    scope.callback(_call(result, 2, wintypes.ULONG), result)  # IUnknown.Release
    return result


def _interface(scope, obj, slot, operation, argtypes=(), args=()):
    method = _call(obj, slot, ctypes.c_long, *argtypes, ctypes.POINTER(_POINTER))
    return _acquire(scope, operation, method, obj, *args)


def _query_interface(scope, obj, iid, name):
    return _interface(scope, obj, 0, f"QueryInterface({name})", (_GUID_REF,), (ctypes.byref(iid),))


def _default_sessions(ole, scope):
    enumerator = _acquire(scope, "CoCreateInstance(MMDeviceEnumerator)", ole.CoCreateInstance,
                          ctypes.byref(_CLSID_DEVICE_ENUMERATOR), None, _CLSCTX_ALL,
                          ctypes.byref(_IID_DEVICE_ENUMERATOR))
    device = _interface(scope, enumerator, 4, "IMMDeviceEnumerator.GetDefaultAudioEndpoint",
                        (ctypes.c_int, ctypes.c_int), (_RENDER, _CONSOLE))
    manager = _interface(scope, device, 3, "IMMDevice.Activate",
                         (_GUID_REF, wintypes.DWORD, _POINTER),
                         (ctypes.byref(_IID_SESSION_MANAGER), _CLSCTX_ALL, None))
    return _interface(scope, manager, 5, "IAudioSessionManager2.GetSessionEnumerator")


def _session_matches(control, pid):
    session_pid = wintypes.DWORD()
    get_pid = _call(control, 14, ctypes.c_long, ctypes.POINTER(wintypes.DWORD))
    # Only S_OK identifies a session owned by a single process.
    return get_pid(control, ctypes.byref(session_pid)) == 0 and session_pid.value == pid


def _matches_mute(volume, muted):
    actual = wintypes.BOOL()
    get_mute = _call(volume, 6, ctypes.c_long, ctypes.POINTER(wintypes.BOOL))
    _check(get_mute(volume, ctypes.byref(actual)), "ISimpleAudioVolume.GetMute")
    if muted is None:
        return bool(actual.value)
    if bool(actual.value) != muted:
        set_mute = _call(volume, 5, ctypes.c_long, wintypes.BOOL, _GUID_REF)
        _check(set_mute(volume, muted, None), "ISimpleAudioVolume.SetMute")
        _check(get_mute(volume, ctypes.byref(actual)), "ISimpleAudioVolume.GetMute")
    return bool(actual.value) == muted


def _session_mute(sessions, index, pid, muted):
    with ExitStack() as scope:
        session = _interface(scope, sessions, 4, "IAudioSessionEnumerator.GetSession",
                             (ctypes.c_int,), (index,))
        control = _query_interface(scope, session, _IID_SESSION_CONTROL, "IAudioSessionControl2")
        if not _session_matches(control, pid):
            return False
        volume = _query_interface(scope, control, _IID_SIMPLE_VOLUME, "ISimpleAudioVolume")
        return _matches_mute(volume, muted)


def _enumerate_and_set(pid: int, muted: bool | None) -> int:
    """Count matching sessions after optionally applying a mute setting."""
    with _com_apartment() as ole, ExitStack() as scope:
        sessions = _default_sessions(ole, scope)
        count = ctypes.c_int()
        get_count = _call(sessions, 3, ctypes.c_long, ctypes.POINTER(ctypes.c_int))
        _check(get_count(sessions, ctypes.byref(count)), "IAudioSessionEnumerator.GetCount")
        return sum(_session_mute(sessions, index, pid, muted) for index in range(count.value))


def read_process_mute(pid: int) -> int:
    """Return the number of matching sessions currently muted; performs no writes."""
    return _enumerate_and_set(pid, None)
