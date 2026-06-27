"""
Ket noi database + dinh nghia 4 bang (dung SQLAlchemy ORM).
Moi class = 1 bang, moi thuoc tinh = 1 cot.
"""
import os
from datetime import datetime
from sqlalchemy import (
    create_engine, String, Integer, Boolean, DateTime, ForeignKey, func
)
from sqlalchemy.orm import (
    DeclarativeBase, Mapped, mapped_column, sessionmaker, relationship
)

DATABASE_URL = os.environ.get(
    "DATABASE_URL", "postgresql+psycopg2://claim:claim123@postgres:5432/claimdb"
)

engine = create_engine(DATABASE_URL, pool_pre_ping=True)
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False)


class Base(DeclarativeBase):
    pass


# 1) users: nguoi dung
class User(Base):
    __tablename__ = "users"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    email: Mapped[str] = mapped_column(String(120), unique=True, index=True)
    password_hash: Mapped[str] = mapped_column(String(200))
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now())


# 2) devices: thiet bi (robot)
class Device(Base):
    __tablename__ = "devices"
    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), unique=True, index=True)
    device_secret: Mapped[str] = mapped_column(String(128), default="")
    claim_code: Mapped[str] = mapped_column(String(16), default="")  # ma de chu nhan may
    owner_id: Mapped[int | None] = mapped_column(ForeignKey("users.id"), nullable=True)
    claimed: Mapped[bool] = mapped_column(Boolean, default=False)
    created_at: Mapped[datetime] = mapped_column(DateTime, server_default=func.now())


# 3) device_users: ai duoc dung thiet bi nao (phan SHARE)
class DeviceUser(Base):
    __tablename__ = "device_users"
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.device_id"), primary_key=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"), primary_key=True)
    role: Mapped[str] = mapped_column(String(16), default="member")  # owner / member


# 4) share_invites: ma moi de share thiet bi (phan SHARE)
class ShareInvite(Base):
    __tablename__ = "share_invites"
    invite_code: Mapped[str] = mapped_column(String(32), primary_key=True)
    device_id: Mapped[str] = mapped_column(String(64), ForeignKey("devices.device_id"))
    created_by: Mapped[int] = mapped_column(ForeignKey("users.id"))
    expire_at: Mapped[datetime] = mapped_column(DateTime)
    used: Mapped[bool] = mapped_column(Boolean, default=False)


# Tien ich: lay 1 phien lam viec DB cho moi request
def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
