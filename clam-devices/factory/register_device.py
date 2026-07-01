#!/usr/bin/env python3
"""
Factory tool: đăng ký (seed) 1 chip DeepLove lên backend.

Gọi POST {BASE}/server-api/devices/manufacture (bảo vệ bằng header
X-API-Key = SERVER_API_KEY). Sau bước này thiết bị mới tồn tại trong DB backend,
có claim_code_hash + public_key + setup_ap_ssid -> app mới createClaimSession được.

Chip tự sinh device_id (từ MAC), claim_code và cặp khóa Ed25519; firmware phơi các
giá trị này ở GET http://192.168.4.1/identity khi đang phát AP setup. Tool này đọc
identity từ chip (hoặc nhận qua tham số) rồi gửi lên backend.

Chỉ dùng thư viện chuẩn Python 3 (không cần pip install).

Ví dụ:
  # Tự đọc identity từ chip (máy tính đã nối vào AP của robot)
  SERVER_API_KEY=xxx ./register_device.py --base https://services.runagent.io

  # Nhập tay
  SERVER_API_KEY=xxx ./register_device.py --base https://services.runagent.io \\
     --device-id ESP32C3_AABBCCDDEEFF --claim-code 7F92KD \\
     --public-key "<ed25519 pubkey base64/PEM>" --setup-ap-ssid Plant-EEFF
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request


def http_get_json(url, timeout=8):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode())


def fetch_identity_from_chip(chip_url):
    return http_get_json(chip_url.rstrip('/') + '/identity')


def register(base, api_key, payload, timeout=15):
    url = base.rstrip('/') + '/server-api/devices/manufacture'
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, method='POST')
    req.add_header('Content-Type', 'application/json')
    req.add_header('X-API-Key', api_key)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors='replace')


def main():
    ap = argparse.ArgumentParser(description='Seed 1 chip DeepLove len backend.')
    ap.add_argument('--base',
                    default=os.getenv('DEEPLOVE_BASE_URL', 'https://services.runagent.io'),
                    help='Base URL backend (mac dinh services.runagent.io)')
    ap.add_argument('--api-key', default=os.getenv('SERVER_API_KEY'),
                    help='SERVER_API_KEY (hoac dat env SERVER_API_KEY)')
    ap.add_argument('--chip-url', default='http://192.168.4.1',
                    help='URL AP cua chip de tu doc /identity')
    ap.add_argument('--device-id')
    ap.add_argument('--claim-code')
    ap.add_argument('--public-key')
    ap.add_argument('--setup-ap-ssid')
    ap.add_argument('--device-name', default='')
    ap.add_argument('--device-type', default='runapet')
    ap.add_argument('--firmware-version', default='')
    args = ap.parse_args()

    if not args.api_key:
        sys.exit('Thieu SERVER_API_KEY (dat env hoac --api-key)')

    device_id = args.device_id
    claim_code = args.claim_code
    public_key = args.public_key
    setup_ap_ssid = args.setup_ap_ssid

    # Neu thieu -> thu doc tu chip qua GET /identity.
    if not (device_id and claim_code and public_key):
        print('Doc identity tu chip %s/identity ...' % args.chip_url)
        try:
            ident = fetch_identity_from_chip(args.chip_url)
        except Exception as e:
            sys.exit('Khong doc duoc identity tu chip: %s\n'
                     'Hay noi may vao AP cua robot, hoac nhap tay '
                     '--device-id/--claim-code/--public-key.' % e)
        device_id = device_id or ident.get('device_id') or ident.get('id')
        claim_code = claim_code or ident.get('claim_code') or ident.get('code')
        public_key = public_key or ident.get('public_key')
        setup_ap_ssid = setup_ap_ssid or ident.get('setup_ap_ssid')

    missing = [k for k, v in {
        'device_id': device_id,
        'claim_code': claim_code,
        'public_key': public_key,
    }.items() if not v]
    if missing:
        sys.exit('Thieu: ' + ', '.join(missing))

    payload = {
        'hardware_id': device_id,
        'claim_code': claim_code,
        'public_key': public_key,
        'setup_ap_ssid': setup_ap_ssid or '',
        'device_name': args.device_name,
        'device_type': args.device_type,
        'firmware_version': args.firmware_version,
    }
    masked = dict(payload, claim_code='***')
    print('Dang ky:', json.dumps(masked, ensure_ascii=False))
    status, resp = register(args.base, args.api_key, payload)
    print('HTTP', status)
    print(resp if isinstance(resp, str)
          else json.dumps(resp, ensure_ascii=False, indent=2))
    sys.exit(0 if status in (200, 201) else 1)


if __name__ == '__main__':
    main()
