"""Link resolution and validation utilities."""

from urllib.parse import parse_qs, urljoin, urlparse


def verify_link(base_url: str, raw_href: str) -> tuple[str, bool]:
    """Resolve a raw href against base_url and validate HTTP(S) or magnet:?xt= URIs.

    Args:
        base_url: The page or site base URL used to resolve relative paths.
        raw_href: The raw ``href`` string extracted from an HTML element.

    Returns:
        A tuple ``(resolved_uri, is_valid)`` where ``resolved_uri`` is the
        resolved URI string and ``is_valid`` indicates whether it is a valid
        HTTP/HTTPS URL or ``magnet:?xt=`` URI.
    """
    cleaned_href = (raw_href or "").strip()
    if not cleaned_href or cleaned_href.startswith("#"):
        return (cleaned_href, False)

    if any(ch in cleaned_href for ch in ("\r", "\n", "\t")):
        return (cleaned_href, False)

    resolved = urljoin((base_url or "").strip(), cleaned_href)
    parsed = urlparse(resolved)
    scheme = parsed.scheme.lower()

    if scheme in {"http", "https"}:
        is_valid = bool(parsed.netloc) and bool(parsed.hostname)
        return (resolved, is_valid)

    if scheme == "magnet":
        query_params = parse_qs(parsed.query, keep_blank_values=False)
        xt_values = query_params.get("xt", [])
        has_xt = any(val.strip() for val in xt_values)
        is_valid = resolved.lower().startswith("magnet:?") and not parsed.path and has_xt
        return (resolved, is_valid)

    return (resolved, False)
