"""
Schemas: dinh nghia hinh dang du lieu vao/ra cua API (Pydantic).
FastAPI dung chung de tu kiem tra du lieu va sinh trang tai lieu.
"""
from datetime import datetime
from pydantic import BaseModel, EmailStr


# ----- Auth -----
class RegisterIn(BaseModel):
    email: EmailStr
    password: str


class LoginIn(BaseModel):
    email: EmailStr
    password: str


class TokenOut(BaseModel):
    access_token: str
    token_type: str = "bearer"
    user_id: int


# ----- Devices -----
class DeviceRegisterIn(BaseModel):
    # Robot tu bao danh tinh len server (giong robot register luc boot)
    device_id: str
    claim_code: str
    device_secret: str = ""


class ClaimIn(BaseModel):
    # User claim thiet bi: gui device_id + claim_code (doc tu QR)
    device_id: str
    claim_code: str


class DeviceOut(BaseModel):
    device_id: str
    claimed: bool
    owner_id: int | None = None
    role: str | None = None


# ----- Share -----
class ShareOut(BaseModel):
    invite_code: str
    device_id: str
    expire_at: datetime


class InviteAcceptIn(BaseModel):
    invite_code: str


class MemberOut(BaseModel):
    user_id: int
    email: str
    role: str
