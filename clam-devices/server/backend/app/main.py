"""
Backend API cho du an Claim Device (FastAPI).
Gom: auth (JWT) + claim + share (co han + huy) + dieu khien qua MQTTS + QR.
"""
import io
import json
import secrets
from datetime import datetime, timedelta

from fastapi import FastAPI, Depends, HTTPException, Response
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from fastapi.staticfiles import StaticFiles
from sqlalchemy.orm import Session
from sqlalchemy import select, delete
import segno

from .db import Base, engine, get_db, User, Device, DeviceUser, ShareInvite
from . import security, mqtts
from .schemas import (
    RegisterIn, LoginIn, TokenOut, DeviceRegisterIn, ClaimIn, DeviceOut,
    ShareIn, ShareOut, InviteAcceptIn, InviteOut, MemberOut, ControlIn,
)

app = FastAPI(title="Claim Device API")
bearer = HTTPBearer()

# claim_code song toi da bao lau (TTL) — qua han phai lam moi
CLAIM_CODE_TTL = timedelta(minutes=10)
# Bang ky tu sinh claim_code (bo O/0/I/1 de khoi nham), giong firmware
CLAIM_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def _gen_claim_code(n: int = 6) -> str:
    return "".join(secrets.choice(CLAIM_ALPHABET) for _ in range(n))


@app.on_event("startup")
def on_startup():
    Base.metadata.create_all(bind=engine)


def get_current_user(
    creds: HTTPAuthorizationCredentials = Depends(bearer),
    db: Session = Depends(get_db),
) -> User:
    uid = security.decode_token(creds.credentials)
    user = db.get(User, uid) if uid is not None else None
    if user is None:
        raise HTTPException(status_code=401, detail="Token khong hop le")
    return user


def can_control(db: Session, device_id: str, user: User) -> bool:
    """User co quyen dieu khien device khong? (owner, hoac member chua het han)"""
    du = db.scalar(
        select(DeviceUser).where(
            DeviceUser.device_id == device_id,
            DeviceUser.user_id == user.id,
        )
    )
    if du is None:
        return False
    if du.role == "owner":
        return True
    if du.expire_at is not None and du.expire_at < datetime.utcnow():
        return False
    return True


def _svg_response(data: str) -> Response:
    qr = segno.make(data, error="m")
    buf = io.BytesIO()
    qr.save(buf, kind="svg", scale=5, border=2)
    return Response(content=buf.getvalue(), media_type="image/svg+xml")


@app.get("/")
def root():
    return {"service": "claim-device-api", "status": "ok"}


@app.get("/mqtt/health")
def mqtt_health():
    """Kiem tra backend co ket noi duoc broker MQTTS khong."""
    return mqtts.health()


# ---------- AUTH ----------
@app.post("/auth/register", response_model=TokenOut)
def register(body: RegisterIn, db: Session = Depends(get_db)):
    if db.scalar(select(User).where(User.email == body.email)):
        raise HTTPException(status_code=400, detail="Email da duoc dang ky")
    user = User(email=body.email, password_hash=security.hash_password(body.password))
    db.add(user)
    db.commit()
    db.refresh(user)
    return TokenOut(access_token=security.create_token(user.id), user_id=user.id)


@app.post("/auth/login", response_model=TokenOut)
def login(body: LoginIn, db: Session = Depends(get_db)):
    user = db.scalar(select(User).where(User.email == body.email))
    if not user or not security.verify_password(body.password, user.password_hash):
        raise HTTPException(status_code=401, detail="Sai email hoac mat khau")
    return TokenOut(access_token=security.create_token(user.id), user_id=user.id)


# ---------- DEVICES ----------
@app.post("/devices/register", response_model=DeviceOut)
def device_register(body: DeviceRegisterIn, db: Session = Depends(get_db)):
    expire = datetime.utcnow() + CLAIM_CODE_TTL
    dev = db.scalar(select(Device).where(Device.device_id == body.device_id))
    if dev is None:
        dev = Device(device_id=body.device_id, claim_code=body.claim_code,
                     device_secret=body.device_secret, claim_code_expire=expire)
        db.add(dev)
    elif not dev.claimed:
        dev.claim_code = body.claim_code
        dev.claim_code_expire = expire        # re-register -> gia han claim_code
        if body.device_secret:
            dev.device_secret = body.device_secret
    db.commit()
    db.refresh(dev)
    return DeviceOut(device_id=dev.device_id, claimed=dev.claimed, owner_id=dev.owner_id)


@app.post("/devices/claim", response_model=DeviceOut)
def device_claim(body: ClaimIn, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    dev = db.scalar(select(Device).where(Device.device_id == body.device_id))
    if dev is None:
        raise HTTPException(status_code=404, detail="Thiet bi chua dang ky tren server")
    if dev.claimed:
        raise HTTPException(status_code=409, detail="Thiet bi da co chu")
    if dev.claim_code_expire and dev.claim_code_expire < datetime.utcnow():
        raise HTTPException(status_code=410, detail="Claim code da het han, hay lam moi ma")
    if not dev.claim_code or dev.claim_code != body.claim_code:
        raise HTTPException(status_code=400, detail="Sai claim code")
    dev.claimed = True
    dev.owner_id = user.id
    dev.claim_code = ""                 # vo hieu ma sau khi claim (dung 1 lan)
    dev.claim_code_expire = None
    db.add(DeviceUser(device_id=dev.device_id, user_id=user.id, role="owner"))
    db.commit()
    return DeviceOut(device_id=dev.device_id, claimed=True, owner_id=user.id, role="owner")


@app.get("/devices", response_model=list[DeviceOut])
def my_devices(user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    rows = db.execute(
        select(Device, DeviceUser.role, DeviceUser.expire_at)
        .join(DeviceUser, DeviceUser.device_id == Device.device_id)
        .where(DeviceUser.user_id == user.id)
    ).all()
    return [
        DeviceOut(device_id=d.device_id, claimed=d.claimed, owner_id=d.owner_id,
                  role=role, expire_at=exp)
        for d, role, exp in rows
    ]


# Lam moi claim_code (chi khi chua co chu) -> sinh ma + han moi
@app.post("/devices/{device_id}/refresh-claim-code")
def refresh_claim_code(device_id: str, db: Session = Depends(get_db)):
    dev = db.scalar(select(Device).where(Device.device_id == device_id))
    if dev is None:
        raise HTTPException(status_code=404, detail="Thiet bi khong ton tai")
    if dev.claimed:
        raise HTTPException(status_code=409, detail="Thiet bi da co chu, khong can lam moi")
    dev.claim_code = _gen_claim_code()
    dev.claim_code_expire = datetime.utcnow() + CLAIM_CODE_TTL
    db.commit()
    return {
        "device_id": dev.device_id,
        "claim_code": dev.claim_code,
        "expire_at": dev.claim_code_expire,
        "ttl_seconds": int(CLAIM_CODE_TTL.total_seconds()),
    }


# QR de claim: ma hoa device_id + claim_code (giong QR robot hien)
@app.get("/devices/{device_id}/claim-qr")
def claim_qr(device_id: str, db: Session = Depends(get_db)):
    dev = db.scalar(select(Device).where(Device.device_id == device_id))
    if dev is None:
        raise HTTPException(status_code=404, detail="Thiet bi khong ton tai")
    if dev.claimed or not dev.claim_code:
        raise HTTPException(status_code=410, detail="Khong co claim_code (da claim hoac chua co)")
    if dev.claim_code_expire and dev.claim_code_expire < datetime.utcnow():
        raise HTTPException(status_code=410, detail="Claim code het han, hay lam moi")
    data = json.dumps({"device_id": dev.device_id, "claim_code": dev.claim_code})
    return _svg_response(data)


# ---------- CONTROL (qua MQTTS) ----------
@app.post("/devices/{device_id}/control")
def control(device_id: str, body: ControlIn,
            user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    if not can_control(db, device_id, user):
        raise HTTPException(status_code=403, detail="Khong co quyen dieu khien (hoac da het han)")
    topic = f"dev/{device_id}/cmd"
    payload = json.dumps(body.command, ensure_ascii=False)
    try:
        mqtts.publish(topic, payload)
    except Exception as e:
        raise HTTPException(status_code=502, detail=f"Khong gui duoc lenh qua MQTT: {e}")
    return {"sent": True, "topic": topic, "command": body.command}


# ---------- SHARE ----------
@app.post("/devices/{device_id}/share", response_model=ShareOut)
def create_share(device_id: str, body: ShareIn = ShareIn(),
                 user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    dev = db.scalar(select(Device).where(Device.device_id == device_id))
    if dev is None:
        raise HTTPException(status_code=404, detail="Thiet bi khong ton tai")
    if dev.owner_id != user.id:
        raise HTTPException(status_code=403, detail="Chi chu so huu moi duoc chia se")

    hours = max(1, body.hours)
    code = "INV-" + secrets.token_hex(3).upper()
    invite = ShareInvite(
        invite_code=code, device_id=device_id, created_by=user.id,
        expire_at=datetime.utcnow() + timedelta(hours=hours), used=False, revoked=False,
    )
    db.add(invite)
    db.commit()
    db.refresh(invite)
    return ShareOut(invite_code=code, device_id=device_id, expire_at=invite.expire_at)


@app.get("/invites/{code}/qr")
def invite_qr(code: str, db: Session = Depends(get_db)):
    inv = db.get(ShareInvite, code)
    if inv is None:
        raise HTTPException(status_code=404, detail="Ma moi khong ton tai")
    return _svg_response(code)


@app.post("/invites/accept", response_model=DeviceOut)
def accept_invite(body: InviteAcceptIn, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    inv = db.get(ShareInvite, body.invite_code)
    if inv is None or inv.used or inv.revoked:
        raise HTTPException(status_code=400, detail="Ma moi khong dung / da dung / da huy")
    if inv.expire_at < datetime.utcnow():
        raise HTTPException(status_code=400, detail="Ma moi da het han")
    if db.scalar(select(DeviceUser).where(
            DeviceUser.device_id == inv.device_id, DeviceUser.user_id == user.id)):
        raise HTTPException(status_code=409, detail="Ban da co quyen dung thiet bi nay")

    # Cap quyen member co han = han cua ma moi, ghi ro do ma nao cap (de huy)
    db.add(DeviceUser(device_id=inv.device_id, user_id=user.id, role="member",
                      expire_at=inv.expire_at, via_invite=inv.invite_code))
    inv.used = True
    db.commit()
    dev = db.scalar(select(Device).where(Device.device_id == inv.device_id))
    return DeviceOut(device_id=dev.device_id, claimed=dev.claimed, owner_id=dev.owner_id,
                     role="member", expire_at=inv.expire_at)


@app.get("/devices/{device_id}/invites", response_model=list[InviteOut])
def list_invites(device_id: str, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    dev = db.scalar(select(Device).where(Device.device_id == device_id))
    if dev is None or dev.owner_id != user.id:
        raise HTTPException(status_code=403, detail="Chi chu so huu moi xem duoc")
    rows = db.scalars(select(ShareInvite).where(ShareInvite.device_id == device_id)).all()
    return [InviteOut(invite_code=i.invite_code, device_id=i.device_id,
                      expire_at=i.expire_at, used=i.used, revoked=i.revoked) for i in rows]


@app.post("/invites/{code}/revoke")
def revoke_invite(code: str, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    """Owner huy ma moi -> nguoi duoc cap qua ma nay MAT QUYEN luon."""
    inv = db.get(ShareInvite, code)
    if inv is None:
        raise HTTPException(status_code=404, detail="Ma moi khong ton tai")
    dev = db.scalar(select(Device).where(Device.device_id == inv.device_id))
    if dev is None or dev.owner_id != user.id:
        raise HTTPException(status_code=403, detail="Chi chu so huu moi duoc huy")
    inv.revoked = True
    # Xoa quyen da cap qua ma nay -> mat quyen dieu khien ngay
    db.execute(delete(DeviceUser).where(DeviceUser.via_invite == code))
    db.commit()
    return {"revoked": code, "message": "Da huy ma moi va thu hoi quyen"}


@app.get("/devices/{device_id}/members", response_model=list[MemberOut])
def list_members(device_id: str, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    me = db.scalar(select(DeviceUser).where(
        DeviceUser.device_id == device_id, DeviceUser.user_id == user.id))
    if me is None:
        raise HTTPException(status_code=403, detail="Ban khong co quyen xem")
    rows = db.execute(
        select(User.id, User.email, DeviceUser.role, DeviceUser.expire_at)
        .join(DeviceUser, DeviceUser.user_id == User.id)
        .where(DeviceUser.device_id == device_id)
    ).all()
    return [MemberOut(user_id=uid, email=email, role=role, expire_at=exp)
            for uid, email, role, exp in rows]


# ---------- WEB APP ----------
app.mount("/app", StaticFiles(directory="app/static", html=True), name="webapp")
