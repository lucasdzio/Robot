"""
Backend API cho du an Claim Device (FastAPI).
Buoc B2: dang ky + dang nhap (JWT). Cac buoc sau them claim/share.
"""
import secrets
from datetime import datetime, timedelta

from fastapi import FastAPI, Depends, HTTPException
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from fastapi.staticfiles import StaticFiles
from sqlalchemy.orm import Session
from sqlalchemy import select

from .db import Base, engine, get_db, User, Device, DeviceUser, ShareInvite
from . import security
from .schemas import (
    RegisterIn, LoginIn, TokenOut, DeviceRegisterIn, ClaimIn, DeviceOut,
    ShareOut, InviteAcceptIn, MemberOut,
)

app = FastAPI(title="Claim Device API")

# "Nguoi gac cong": doc header Authorization: Bearer <token>
bearer = HTTPBearer()


def get_current_user(
    creds: HTTPAuthorizationCredentials = Depends(bearer),
    db: Session = Depends(get_db),
) -> User:
    """Giai ma JWT -> tra ve User dang dang nhap. Sai token -> 401."""
    uid = security.decode_token(creds.credentials)
    user = db.get(User, uid) if uid is not None else None
    if user is None:
        raise HTTPException(status_code=401, detail="Token khong hop le")
    return user


@app.on_event("startup")
def on_startup():
    # Tu tao 4 bang trong DB neu chua co
    Base.metadata.create_all(bind=engine)


@app.get("/")
def root():
    return {"service": "claim-device-api", "status": "ok"}


# ---------- AUTH ----------
@app.post("/auth/register", response_model=TokenOut)
def register(body: RegisterIn, db: Session = Depends(get_db)):
    # Email da ton tai chua?
    existing = db.scalar(select(User).where(User.email == body.email))
    if existing:
        raise HTTPException(status_code=400, detail="Email da duoc dang ky")

    user = User(
        email=body.email,
        password_hash=security.hash_password(body.password),
    )
    db.add(user)
    db.commit()
    db.refresh(user)

    token = security.create_token(user.id)
    return TokenOut(access_token=token, user_id=user.id)


@app.post("/auth/login", response_model=TokenOut)
def login(body: LoginIn, db: Session = Depends(get_db)):
    user = db.scalar(select(User).where(User.email == body.email))
    if not user or not security.verify_password(body.password, user.password_hash):
        raise HTTPException(status_code=401, detail="Sai email hoac mat khau")

    token = security.create_token(user.id)
    return TokenOut(access_token=token, user_id=user.id)


# ---------- DEVICES ----------
@app.post("/devices/register", response_model=DeviceOut)
def device_register(body: DeviceRegisterIn, db: Session = Depends(get_db)):
    """Robot tu bao danh tinh len server (khong can dang nhap user)."""
    dev = db.scalar(select(Device).where(Device.device_id == body.device_id))
    if dev is None:
        dev = Device(
            device_id=body.device_id,
            claim_code=body.claim_code,
            device_secret=body.device_secret,
        )
        db.add(dev)
    elif not dev.claimed:
        # Chua co chu -> cap nhat claim_code moi (vd robot reset sinh ma moi)
        dev.claim_code = body.claim_code
        if body.device_secret:
            dev.device_secret = body.device_secret
    db.commit()
    db.refresh(dev)
    return DeviceOut(device_id=dev.device_id, claimed=dev.claimed, owner_id=dev.owner_id)


@app.post("/devices/claim", response_model=DeviceOut)
def device_claim(
    body: ClaimIn,
    user: User = Depends(get_current_user),
    db: Session = Depends(get_db),
):
    """User (da dang nhap) claim thiet bi bang device_id + claim_code."""
    dev = db.scalar(select(Device).where(Device.device_id == body.device_id))
    if dev is None:
        raise HTTPException(status_code=404, detail="Thiet bi chua dang ky tren server")
    if dev.claimed:
        raise HTTPException(status_code=409, detail="Thiet bi da co chu")
    if dev.claim_code != body.claim_code:
        raise HTTPException(status_code=400, detail="Sai claim code")

    # Thanh cong: gan chu so huu + them vao device_users (role owner)
    dev.claimed = True
    dev.owner_id = user.id
    db.add(DeviceUser(device_id=dev.device_id, user_id=user.id, role="owner"))
    db.commit()
    db.refresh(dev)
    return DeviceOut(device_id=dev.device_id, claimed=True, owner_id=user.id, role="owner")


@app.get("/devices", response_model=list[DeviceOut])
def my_devices(user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    """Danh sach thiet bi ma user nay duoc dung (owner hoac member)."""
    rows = db.execute(
        select(Device, DeviceUser.role)
        .join(DeviceUser, DeviceUser.device_id == Device.device_id)
        .where(DeviceUser.user_id == user.id)
    ).all()
    return [
        DeviceOut(device_id=d.device_id, claimed=d.claimed, owner_id=d.owner_id, role=role)
        for d, role in rows
    ]


# ---------- SHARE ----------
@app.post("/devices/{device_id}/share", response_model=ShareOut)
def create_share(device_id: str, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    """Owner tao ma moi (INV-xxxxx) de chia se thiet bi, han 24 gio."""
    dev = db.scalar(select(Device).where(Device.device_id == device_id))
    if dev is None:
        raise HTTPException(status_code=404, detail="Thiet bi khong ton tai")
    if dev.owner_id != user.id:
        raise HTTPException(status_code=403, detail="Chi chu so huu moi duoc chia se")

    code = "INV-" + secrets.token_hex(3).upper()   # vd: INV-9A3F2C
    invite = ShareInvite(
        invite_code=code,
        device_id=device_id,
        created_by=user.id,
        expire_at=datetime.utcnow() + timedelta(hours=24),
        used=False,
    )
    db.add(invite)
    db.commit()
    db.refresh(invite)
    return ShareOut(invite_code=code, device_id=device_id, expire_at=invite.expire_at)


@app.post("/invites/accept", response_model=DeviceOut)
def accept_invite(body: InviteAcceptIn, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    """User khac nhap ma moi -> tro thanh member cua thiet bi."""
    inv = db.get(ShareInvite, body.invite_code)
    if inv is None or inv.used:
        raise HTTPException(status_code=400, detail="Ma moi khong dung hoac da su dung")
    if inv.expire_at < datetime.utcnow():
        raise HTTPException(status_code=400, detail="Ma moi da het han")

    # Da co quyen chua?
    existing = db.scalar(
        select(DeviceUser).where(
            DeviceUser.device_id == inv.device_id,
            DeviceUser.user_id == user.id,
        )
    )
    if existing:
        raise HTTPException(status_code=409, detail="Ban da co quyen dung thiet bi nay")

    db.add(DeviceUser(device_id=inv.device_id, user_id=user.id, role="member"))
    inv.used = True   # ma moi dung 1 lan
    db.commit()

    dev = db.scalar(select(Device).where(Device.device_id == inv.device_id))
    return DeviceOut(device_id=dev.device_id, claimed=dev.claimed, owner_id=dev.owner_id, role="member")


@app.get("/devices/{device_id}/members", response_model=list[MemberOut])
def list_members(device_id: str, user: User = Depends(get_current_user), db: Session = Depends(get_db)):
    """Xem ai dang duoc dung thiet bi (chi owner/member moi xem duoc)."""
    me = db.scalar(
        select(DeviceUser).where(
            DeviceUser.device_id == device_id,
            DeviceUser.user_id == user.id,
        )
    )
    if me is None:
        raise HTTPException(status_code=403, detail="Ban khong co quyen xem")

    rows = db.execute(
        select(User.id, User.email, DeviceUser.role)
        .join(DeviceUser, DeviceUser.user_id == User.id)
        .where(DeviceUser.device_id == device_id)
    ).all()
    return [MemberOut(user_id=uid, email=email, role=role) for uid, email, role in rows]


# ---------- WEB APP (frontend tinh) ----------
# Phuc vu trang web tai /app/  (cung origin -> khong vuong CORS)
app.mount("/app", StaticFiles(directory="app/static", html=True), name="webapp")
