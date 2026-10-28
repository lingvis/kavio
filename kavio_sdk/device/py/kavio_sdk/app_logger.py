"""Kavio Device App logger: daily UTC files, 72h retention, one startup session."""
import datetime
import logging
import os
import re
import threading
import time

_LOCK = threading.RLock()
_SESSION = None
_LOGGER = None
_HANDLER = None
_CLEANUP_THREAD = None
_STOP = threading.Event()
_NAME = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
_SESSION_FILE = re.compile(r"^[A-Za-z0-9_-]{1,64}_[0-9]{8}_[0-9]{6}_[0-9]{3}_([0-9]+)(?:_[0-9]+)?\.log$")


class _UTCFormatter(logging.Formatter):
    converter = time.gmtime

    def formatTime(self, record, datefmt=None):
        dt = datetime.datetime.fromtimestamp(record.created, datetime.timezone.utc)
        return dt.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % record.msecs


def _cleanup(directory, active):
    now = time.time()
    try:
        for name in os.listdir(directory):
            match = _SESSION_FILE.fullmatch(name)
            if not match or name == active:
                continue
            path = os.path.join(directory, name)
            try:
                if not os.path.islink(path) and os.path.isfile(path) and now - os.path.getmtime(path) > 72 * 3600:
                    os.unlink(path)
            except OSError:
                pass
    except OSError:
        pass


def _cleanup_worker(directory):
    while not _STOP.wait(3600):
        handler = _HANDLER
        _cleanup(directory, handler.current_name if handler is not None else "")


def _utc_day(timestamp):
    return datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime("%Y%m%d")


def _new_filename(directory, name, timestamp):
    dt = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc)
    base = "%s_%03d_%d" % (dt.strftime("%Y%m%d_%H%M%S"), dt.microsecond // 1000, os.getpid())
    suffix = 0
    filename = "%s_%s.log" % (name, base)
    while os.path.exists(os.path.join(directory, filename)):
        suffix += 1
        filename = "%s_%s_%d.log" % (name, base, suffix)
    return filename, base


class _DailyFileHandler(logging.FileHandler):
    """Rotate to a fresh Kavio-format file on UTC day changes; no size splitting."""

    def __init__(self, directory, name):
        self.directory = directory
        self.stem = name
        now = time.time()
        self.day = _utc_day(now)
        self.current_name, self.startup_session = _new_filename(directory, name, now)
        super().__init__(os.path.join(directory, self.current_name), encoding="utf-8")
        self._write_session_header()

    def _write_session_header(self):
        # Written independently of the user-selected minimum log level.
        record = logging.LogRecord("kavio.logging", logging.INFO, __file__, 0,
                                   "[KavioLogging] session_id=%s file=%s",
                                   (self.startup_session, self.current_name), None)
        logging.FileHandler.emit(self, record)

    def rotate_for(self, timestamp):
        next_day = _utc_day(timestamp)
        if self.day == next_day:
            return
        filename, _ = _new_filename(self.directory, self.stem, timestamp)
        path = os.path.abspath(os.path.join(self.directory, filename))
        # Open first so failure does not abandon the still-working old file.
        # FileHandler._open() requires baseFilename to point at the new location.
        old_path = self.baseFilename
        self.baseFilename = path
        try:
            next_stream = self._open()
        except Exception:
            self.baseFilename = old_path
            return
        old_stream, self.stream = self.stream, next_stream
        self.day, self.current_name = next_day, filename
        if old_stream is not None:
            old_stream.flush()
            old_stream.close()
        self._write_session_header()

    def emit(self, record):
        self.rotate_for(record.created)
        super().emit(record)

def init(name="main", level="INFO"):
    """Initialize once; name is a filename stem, never a path."""
    global _LOGGER, _HANDLER, _SESSION, _CLEANUP_THREAD
    if not isinstance(name, str) or not _NAME.fullmatch(name):
        raise ValueError("Log name must match [A-Za-z0-9_-]{1,64}")
    directory = os.environ.get("KAVIO_APP_LOG_DIR")
    if not directory:
        raise RuntimeError("KAVIO_APP_LOG_DIR is required")
    with _LOCK:
        if _LOGGER is not None:
            if _LOGGER.name != "kavio.app.%s.%s" % (os.getpid(), name):
                raise RuntimeError("Logger already initialized with another name")
            return _LOGGER
        os.makedirs(directory, exist_ok=True)
        handler = _DailyFileHandler(directory, name)
        handler.setFormatter(_UTCFormatter(
            "[%(asctime)s] [%(process)d:%(thread)d] [%(levelname)s] [%(filename)s:%(lineno)d] %(message)s"))
        logger = logging.getLogger("kavio.app.%s.%s" % (os.getpid(), name))
        logger.setLevel(level)
        logger.propagate = False
        logger.addHandler(handler)
        _HANDLER, _LOGGER, _SESSION = handler, logger, handler.startup_session
        _cleanup(directory, handler.current_name)
        _STOP.clear()
        _CLEANUP_THREAD = threading.Thread(target=_cleanup_worker, args=(directory,), daemon=True)
        _CLEANUP_THREAD.start()
        return logger


def current_filename():
    with _LOCK:
        return _HANDLER.current_name if _HANDLER is not None else None


def session_id():
    with _LOCK:
        return _SESSION


def shutdown():
    global _LOGGER, _HANDLER, _SESSION, _CLEANUP_THREAD
    with _LOCK:
        _STOP.set()
        if _CLEANUP_THREAD is not None and _CLEANUP_THREAD.is_alive():
            _CLEANUP_THREAD.join(timeout=2)
        if _HANDLER:
            _HANDLER.flush()
            _HANDLER.close()
            _LOGGER.removeHandler(_HANDLER)
        _HANDLER, _LOGGER, _SESSION, _CLEANUP_THREAD = None, None, None, None
