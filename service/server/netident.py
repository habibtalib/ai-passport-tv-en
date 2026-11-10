"""Auto-detect the server address the device should be given.

Two forms are offered:

* Name (`<hostname>.local`): enter this on the device. It stays valid after the router reassigns the IP.
* Numeric address (`192.168.1.20`): socket binding needs a literal address; it cannot listen on a name.

Name lookup differs per OS, so each way is tried in turn; on failure it reports "name unknown" and never guesses, since a wrong name is worse than none.
"""

from __future__ import annotations

import platform
import socket
import subprocess

# The standard library cannot enumerate network interfaces. Connecting a UDP socket to a remote
# address sends no packet, but the kernel picks the outgoing interface, so the socket's local
# address is enough. Reserved documentation addresses are preferred so a stray packet reaches no host.
_PROBE_ADDRESSES = (
    ("192.0.2.1", 9),        # RFC 5737 TEST-NET-1
    ("198.51.100.1", 9),     # RFC 5737 TEST-NET-2
    ("8.8.8.8", 53),         # last resort: an address that always has a route
)


def lan_address() -> str | None:
    """IPv4 address that devices on the same network use to reach this machine; None if there is no outgoing route.

    Loopback is not used as a substitute: 127.0.0.1 is reachable only from this machine, and entering it on the device looks like a server fault.
    """
    for address, port in _PROBE_ADDRESSES:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(0.5)
            sock.connect((address, port))
            found = sock.getsockname()[0]
        except OSError:
            continue
        finally:
            sock.close()
        # A loopback address means the probe found nothing usable; try the next one.
        if found and not found.startswith("127."):
            return found
    return None


def _run(command: list[str]) -> str | None:
    """Return the first line of the command's output, or None on failure; never raises.

    UTF-8 is set explicitly: the machine name may contain non-ASCII characters, and reading with the platform default encoding could raise in the reader thread.
    """
    try:
        done = subprocess.run(command, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=5)
    except (OSError, subprocess.SubprocessError):
        return None
    if done.returncode != 0:
        return None
    line = done.stdout.strip().splitlines()
    return line[0].strip() if line else None


def local_name() -> str | None:
    """This machine's `.local` name, or None if it cannot be determined.

    * macOS: `scutil` gives the Bonjour name.
    * Linux/BSD: `avahi-resolve` asks the running mDNS service; without avahi there is no name.
    * Windows: take the computer name from the environment; the system publishes mDNS as `<name>.local`.

    Only names with the `.local` suffix are returned; a bare hostname does not resolve on home networks.
    """
    system = platform.system()

    if system == "Darwin":
        name = _run(["scutil", "--get", "LocalHostName"])
        return f"{name}.local" if name else None

    if system == "Windows":
        import os
        name = os.environ.get("COMPUTERNAME")
        return f"{name}.local" if name else None

    # Linux, BSD and other systems running an mDNS service.
    name = _run(["avahi-resolve", "--address", "-n", socket.gethostname()])
    if name:
        return name.rstrip(".") + ".local" if not name.endswith(".local") else name
    return None


def describe(address: str | None = None, port: int = 8096) -> list[str]:
    """Address hint lines printed at startup.

    `address` is the LAN address shown as a fallback; None when the caller found none (e.g. listening on all interfaces with no private address).
    """
    name = local_name()

    # The listen address is not printed: the device is given the name, and the numeric address appears only as a fallback when the name fails.
    lines: list[str] = []
    if not address:
        if name:
            lines.append("Address to enter on the device:")
            lines.append(f"    {name}:{port}")
            lines.append(f"(if it cannot connect, use this computer's LAN address:{port})")
        else:
            lines.append(f"Could not detect this computer's LAN address. Enter its LAN address on the device:{port}.")
        lines.append("")
        return lines

    # Devices on the LAN cannot connect to a loopback binding, so the entry advice below does not apply; explain why instead.
    if address.startswith("127."):
        lines.append("This is a local-only address (starts with 127); only this computer can reach it,")
        lines.append("so devices on the LAN cannot connect.")
        return lines

    # Give a single value to enter, with at most one fallback line below.
    if name:
        lines.append("Address to enter on the device:")
        lines.append(f"    {name}:{port}")
        lines.append(f"(if it cannot connect, use {address}:{port})")
    else:
        lines.append("Address to enter on the device:")
        lines.append(f"    {address}:{port}")
        lines.append("(this computer's name could not be read; re-enter it after the router changes the IP)")
    # Trailing blank line separates later output from the entry instructions.
    lines.append("")
    return lines
