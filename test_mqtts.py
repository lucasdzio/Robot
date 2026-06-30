#!/usr/bin/env python3
"""Publish-and-subscribe smoke test for an MQTT-over-TLS listener.

The script uses only Python's standard library. It connects using MQTT 3.1.1,
subscribes to a unique topic, publishes a payload to that topic, and verifies
that it receives the same payload back.
"""

import argparse
import os
import socket
import ssl
import sys
import time
import uuid
import traceback


def mqtt_string(value):
    encoded = value.encode("utf-8")
    return len(encoded).to_bytes(2, "big") + encoded


def remaining_length(value):
    encoded = bytearray()
    while True:
        byte = value % 128
        value //= 128
        if value:
            byte |= 0x80
        encoded.append(byte)
        if not value:
            return bytes(encoded)


def packet(packet_type, body):
    return bytes([packet_type]) + remaining_length(len(body)) + body


def read_exact(connection, length):
    data = bytearray()
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise RuntimeError("broker closed the connection")
        data.extend(chunk)
    return bytes(data)


def read_packet(connection):
    first_byte = read_exact(connection, 1)[0]
    length = 0
    multiplier = 1
    while True:
        byte = read_exact(connection, 1)[0]
        length += (byte & 0x7F) * multiplier
        if not byte & 0x80:
            break
        multiplier *= 128
        if multiplier > 128**3:
            raise RuntimeError("invalid MQTT remaining length")
    return first_byte, read_exact(connection, length)


def describe_certificate(certificate):
    """Return the useful verification fields from SSLSocket.getpeercert()."""
    def format_name(name):
        # Python returns certificate names as RDN groups, for example:
        # ((('commonName', 'mqtt.runagent.io'),),)
        return ", ".join(
            f"{key}={value}"
            for group in name
            for key, value in group
        )

    return {
        "subject": format_name(certificate.get("subject", ())),
        "issuer": format_name(certificate.get("issuer", ())),
        "notAfter": certificate.get("notAfter", "unknown"),
        "subjectAltName": ", ".join(value for _, value in certificate.get("subjectAltName", ())),
    }


def debug(args, message):
    if args.debug:
        print(f"DEBUG: {message}", file=sys.stderr, flush=True)


def info(message):
    print(f"INFO: {message}", file=sys.stderr, flush=True)


CONNACK_ERRORS = {
    1: "unacceptable protocol version",
    2: "client identifier rejected",
    3: "server unavailable",
    4: "bad username or password",
    5: "not authorized",
}


def connect_packet(client_id, username, password):
    flags = 0x02  # Clean session.
    payload = mqtt_string(client_id)
    if username is not None:
        flags |= 0x80
        payload += mqtt_string(username)
    if password is not None:
        flags |= 0x40
        payload += mqtt_string(password)
    variable_header = mqtt_string("MQTT") + bytes([4, flags]) + (30).to_bytes(2, "big")
    return packet(0x10, variable_header + payload)


def wait_for(connection, expected_type, timeout, args):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        connection.settimeout(max(0.1, deadline - time.monotonic()))
        try:
            packet_type, body = read_packet(connection)
        except TimeoutError as error:
            raise RuntimeError(f"timed out waiting for MQTT packet type {expected_type}") from error
        debug(args, f"received MQTT packet type={packet_type >> 4} flags=0x{packet_type & 0x0F:x} body={body.hex()}")
        if packet_type >> 4 == expected_type:
            return packet_type, body
    raise RuntimeError(f"timed out waiting for MQTT packet type {expected_type}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.getenv("MQTTS_HOST", "mqtt.runagent.io"))
    parser.add_argument("--port", type=int, default=int(os.getenv("MQTTS_PORT", "8883")))
    parser.add_argument(
        "--ca-file",
        default=os.getenv("MQTTS_CA_FILE"),
        help="custom CA certificate path; omit for a publicly trusted certificate",
    )
    parser.add_argument("--username", default='sonnh')
    parser.add_argument("--password", default='son123')
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument(
        "--debug",
        action="store_true",
        help="show DNS, TCP, TLS, and MQTT protocol diagnostics (never prints the password)",
    )
    args = parser.parse_args()

    topic = f"deeplove/smoke-test/{uuid.uuid4()}"
    payload = f"mqtts-ok-{uuid.uuid4()}".encode("utf-8")
    client_id = f"deeplove-mqtts-test-{uuid.uuid4().hex[:12]}"

    # mqtt.runagent.io is expected to present a publicly trusted certificate,
    # so the platform trust store is used by default.  Supplying --ca-file
    # keeps the test usable for private/internal PKI deployments as well.
    tls_context = ssl.create_default_context(cafile=args.ca_file)
    info(f"using MQTTS endpoint mqtts://{args.host}:{args.port}")
    info("transport mode: TCP socket first, then TLS handshake, then MQTT 3.1.1 over TLS")
    debug(args, f"target={args.host}:{args.port}, timeout={args.timeout}s")
    debug(args, f"TLS verification: check_hostname={tls_context.check_hostname}, verify_mode={tls_context.verify_mode}")
    if args.ca_file:
        debug(args, f"custom CA file: {args.ca_file}")

    try:
        addresses = socket.getaddrinfo(args.host, args.port, type=socket.SOCK_STREAM)
        resolved = ", ".join(sorted({address[4][0] for address in addresses}))
        debug(args, f"DNS resolved {args.host} -> {resolved}")
    except socket.gaierror as error:
        raise RuntimeError(f"DNS lookup failed for {args.host}: {error}") from error

    started = time.monotonic()
    with socket.create_connection((args.host, args.port), timeout=args.timeout) as tcp:
        debug(args, f"TCP connected local={tcp.getsockname()} peer={tcp.getpeername()} in {time.monotonic() - started:.2f}s")
        debug(args, "wrapping TCP socket with TLS before sending any MQTT packet")
        with tls_context.wrap_socket(tcp, server_hostname=args.host) as connection:
            certificate = describe_certificate(connection.getpeercert())
            info(
                f"TLS established for MQTTS: version={connection.version()} "
                f"cipher={connection.cipher()[0]} peer={args.host}:{args.port}"
            )
            info("confirmed: MQTT CONNECT/SUBSCRIBE/PUBLISH will be sent through the TLS socket, not plain MQTT")
            debug(
                args,
                "TLS handshake OK "
                f"version={connection.version()} cipher={connection.cipher()[0]} "
                f"subject={certificate['subject']!r} issuer={certificate['issuer']!r} "
                f"expires={certificate['notAfter']!r} SANs={certificate['subjectAltName']!r}",
            )
            debug(args, f"sending MQTT CONNECT over MQTTS client_id={client_id!r} username={args.username!r}")
            connection.sendall(connect_packet(client_id, args.username, args.password))
            _, connack = wait_for(connection, 2, args.timeout, args)
            if len(connack) != 2 or connack[1] != 0:
                code = connack[1] if len(connack) > 1 else "missing"
                reason = CONNACK_ERRORS.get(code, "malformed CONNACK")
                raise RuntimeError(f"MQTT CONNECT rejected (code: {code}; {reason})")
            debug(args, "MQTT CONNACK accepted")

            packet_id = b"\x00\x01"
            debug(args, f"sending MQTT SUBSCRIBE over MQTTS topic={topic!r}")
            connection.sendall(packet(0x82, packet_id + mqtt_string(topic) + b"\x00"))
            _, suback = wait_for(connection, 9, args.timeout, args)
            if suback != packet_id + b"\x00":
                raise RuntimeError(f"MQTT SUBSCRIBE rejected: {suback.hex()}")
            debug(args, "MQTT SUBACK accepted (QoS 0)")

            debug(args, f"sending MQTT PUBLISH over MQTTS topic={topic!r}, payload_bytes={len(payload)}")
            connection.sendall(packet(0x30, mqtt_string(topic) + payload))
            _, published = wait_for(connection, 3, args.timeout, args)
            topic_length = int.from_bytes(published[:2], "big")
            received_topic = published[2 : 2 + topic_length].decode("utf-8")
            received_payload = published[2 + topic_length :]
            if received_topic != topic or received_payload != payload:
                raise RuntimeError("received a different MQTT message than the one published")

            connection.sendall(b"\xe0\x00")  # MQTT DISCONNECT

    print(f"PASS: MQTT over TLS works at {args.host}:{args.port}")
    print(f"      topic={topic}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ssl.SSLError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        if "--debug" in sys.argv:
            traceback.print_exc()
        sys.exit(1)

# MQTTS smoke test for mqtt.runagent.io
# python3 test_mqtts.py --host mqtt.runagent.io --debug

