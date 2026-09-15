"""Shared helpers for version values coming from PlatformIO options."""


def normalize_config_string(value):
    """Return a PlatformIO string option without its configuration quote layers."""
    text = "" if value is None else str(value).strip()
    while len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        text = text[1:-1].strip()
    return text
