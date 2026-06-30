"""
MQTTS client toi gian (MQTT 3.1.1 over TLS) - chi dung thu vien chuan Python.
Tich hop tu test_mqtts.py: dung de BACKEND gui lenh dieu khien xuong thiet bi.

Cau hinh qua bien moi truong:
  MQTTS_HOST (mac dinh mqtt.runagent.io), MQTTS_PORT (8883),
  MQTTS_USER, MQTTS_PASS, MQTTS_CA_FILE (tuy chon)
"""
import os
import socket
import ssl
import uuid

CONNACK_ERRORS = {
    1: "unacceptable protocol version",
    2: "client identifier rejected",
    3: "server unavailable",
    4: "bad username or password",
    5: "not authorized",
}


def _mqtt_string(value):
    b = value.encode("utf-8")
    return len(b).to_bytes(2, "big") + b


def _remaining_length(n):
    out = bytearray()
    while True:
        byte = n % 128
        n //= 128
        if n:
            byte |= 0x80
        out.append(byte)
        if not n:
            return bytes(out)


def _packet(packet_type, body):
    return bytes([packet_type]) + _remaining_length(len(body)) + body


def _connect_packet(client_id, username, password):
    flags = 0x02  # clean session
    payload = _mqtt_string(client_id)
    if username is not None:
        flags |= 0x80
        payload += _mqtt_string(username)
    if password is not None:
        flags |= 0x40
        payload += _mqtt_string(password)
    vh = _mqtt_string("MQTT") + bytes([4, flags]) + (30).to_bytes(2, "big")
    return _packet(0x10, vh + payload)


def _read_exact(conn, n):
    data = bytearray()
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise RuntimeError("broker dong ket noi")
        data.extend(chunk)
    return bytes(data)


def _read_packet(conn):
    first = _read_exact(conn, 1)[0]
    length = 0
    mul = 1
    while True:
        byte = _read_exact(conn, 1)[0]
        length += (byte & 0x7F) * mul
        if not byte & 0x80:
            break
        mul *= 128
    return first, _read_exact(conn, length)


def publish(topic: str, payload, timeout: float = 10) -> bool:
    """Gui 1 tin (QoS0) len broker MQTTS. Nem loi neu khong ket noi/dang nhap duoc."""
    host = os.getenv("MQTTS_HOST", "mqtt.runagent.io")
    port = int(os.getenv("MQTTS_PORT", "8883"))
    user = os.getenv("MQTTS_USER") or None
    pw = os.getenv("MQTTS_PASS") or None
    ca = os.getenv("MQTTS_CA_FILE") or None
    client_id = "claim-backend-" + uuid.uuid4().hex[:10]

    ctx = ssl.create_default_context(cafile=ca)
    with socket.create_connection((host, port), timeout=timeout) as tcp:
        with ctx.wrap_socket(tcp, server_hostname=host) as conn:
            conn.settimeout(timeout)
            conn.sendall(_connect_packet(client_id, user, pw))
            ft, body = _read_packet(conn)
            if ft >> 4 != 2 or len(body) < 2 or body[1] != 0:
                code = body[1] if len(body) > 1 else "?"
                raise RuntimeError(f"CONNECT bi tu choi (code {code}: {CONNACK_ERRORS.get(code, 'loi')})")
            data = payload.encode("utf-8") if isinstance(payload, str) else payload
            conn.sendall(_packet(0x30, _mqtt_string(topic) + data))  # PUBLISH QoS0
            conn.sendall(b"\xe0\x00")  # DISCONNECT
    return True


def health() -> dict:
    """Thu ket noi broker, tra ve trang thai (de App kiem tra)."""
    host = os.getenv("MQTTS_HOST", "mqtt.runagent.io")
    port = int(os.getenv("MQTTS_PORT", "8883"))
    try:
        publish("backend/health", "ping")
        return {"ok": True, "broker": f"{host}:{port}"}
    except Exception as e:
        return {"ok": False, "broker": f"{host}:{port}", "error": str(e)}
