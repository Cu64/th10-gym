"""Preserve the error that triggered resource cleanup."""


def cleanup_after_error(cleanup, error: BaseException, label: str) -> None:
    try:
        cleanup()
    except BaseException as cleanup_error:
        error.add_note(f"{label}: {cleanup_error!r}")
