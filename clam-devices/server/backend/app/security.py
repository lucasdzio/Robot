"""
Bao mat: bam mat khau (bcrypt) + tao/kiem JWT token.
"""
import os
import datetime
import bcrypt
import jwt

JWT_SECRET = os.environ.get("JWT_SECRET", "dev-secret-doi-di")
JWT_ALGO = "HS256"
JWT_EXPIRE_HOURS = 24 * 7  # token song 7 ngay


def hash_password(plain: str) -> str:
    """Bam mat khau thanh chuoi an toan (khong the doi nguoc)."""
    return bcrypt.hashpw(plain.encode(), bcrypt.gensalt()).decode()


def verify_password(plain: str, hashed: str) -> bool:
    """Kiem mat khau nguoi dung nhap co khop voi ban bam khong."""
    try:
        return bcrypt.checkpw(plain.encode(), hashed.encode())
    except ValueError:
        return False


def create_token(user_id: int) -> str:
    """Tao JWT token chua user_id, dung de chung minh 'toi la ai'."""
    payload = {
        "sub": str(user_id),
        "exp": datetime.datetime.utcnow() + datetime.timedelta(hours=JWT_EXPIRE_HOURS),
    }
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALGO)


def decode_token(token: str) -> int | None:
    """Giai ma token -> tra ve user_id, hoac None neu token sai/het han."""
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGO])
        return int(payload["sub"])
    except Exception:
        return None
