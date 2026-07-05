"""
Discord login / token management.

Two ways to get a token:
  1. Browser login — opens Discord in a browser, you log in, token is captured
  2. Manual paste  — you paste the token from browser DevTools (fallback if
                     Playwright isn't installed or you prefer not to use it)

The token is saved locally in .token so you only log in once.
If the token expires (e.g. password change), the companion detects it
and re-prompts.
"""

import json
import logging
import time

from paths import BASE_DIR

log = logging.getLogger("dawncord.auth")

TOKEN_FILE = BASE_DIR / ".token"


def get_token() -> str:
    """Return a valid Discord user token, prompting login if needed."""
    saved = _load_saved_token()
    if saved:
        log.info("Using saved token.")
        return saved

    token = _acquire_token()
    _save_token(token)
    return token


def invalidate_and_relogin() -> str:
    """Token was rejected by Discord. Clear it and get a new one."""
    log.warning("Saved token is no longer valid. Need to re-login.")
    clear_token()
    token = _acquire_token()
    _save_token(token)
    return token


def clear_token():
    """Remove saved token (for logout / re-login)."""
    if TOKEN_FILE.exists():
        TOKEN_FILE.unlink()
        log.info("Saved token removed.")


# Non-interactive pieces for the windowed companion (gui.py), which asks
# for the token with its own dialog instead of input().

def load_saved_token() -> str | None:
    return _load_saved_token()


def save_token(token: str):
    _save_token(token)


def _acquire_token() -> str:
    """Try browser login first, fall back to manual paste."""
    # Try Playwright (browser login)
    try:
        from playwright.sync_api import sync_playwright  # noqa: F401
        print("\n[1] Browser login (recommended)")
        print("[2] Paste token manually")
        choice = input("\nChoose login method [1]: ").strip() or "1"
    except ImportError:
        log.info("Playwright not installed — using manual token entry.")
        choice = "2"

    if choice == "1":
        # Playwright being importable doesn't mean its browser is installed
        # (pip install playwright is only half the job). Any failure here
        # must degrade to manual entry, not crash.
        try:
            token = _browser_login()
        except Exception as e:
            log.warning("Browser login failed: %s", e)
            print("\n  Browser login failed. If Chromium is missing, install it with:")
            print("      python -m playwright install chromium")
            print("  Falling back to manual token entry.\n")
            token = None
        if token:
            log.info("Token captured from browser.")
            return token
        print("Browser login didn't capture a token. Falling back to manual.")

    return _manual_token()


def _manual_token() -> str:
    """Guide the user to find and paste their token."""
    print("\n" + "=" * 55)
    print("  HOW TO GET YOUR DISCORD TOKEN")
    print("=" * 55)
    print("  1. Open Discord in your browser (discord.com/app)")
    print("  2. Press F12 to open DevTools")
    print("  3. Go to the Network tab")
    print("  4. Type 'api' in the filter bar")
    print("  5. Click on any request to discord.com/api/...")
    print("  6. In the Headers, find 'authorization'")
    print("  7. Copy that value (it's your token)")
    print("=" * 55)
    token = input("\nPaste your token here: ").strip()
    if not token:
        raise RuntimeError("No token provided.")
    return token


def _load_saved_token() -> str | None:
    if TOKEN_FILE.exists():
        try:
            data = json.loads(TOKEN_FILE.read_text())
            return data.get("token")
        except (json.JSONDecodeError, KeyError):
            log.warning("Corrupt token file, removing.")
            TOKEN_FILE.unlink()
    return None


def _save_token(token: str):
    TOKEN_FILE.write_text(json.dumps({"token": token}))
    log.info("Token saved to %s", TOKEN_FILE)


def _browser_login() -> str | None:
    """
    Open Discord login in a real browser window.
    Intercept the Authorization header from API requests after login.
    """
    from playwright.sync_api import sync_playwright

    captured_token = None

    def on_request(request):
        nonlocal captured_token
        if captured_token:
            return
        # Only intercept Discord API calls
        if "discord.com/api" not in request.url:
            return
        auth = request.headers.get("authorization")
        if auth and not auth.startswith("Bot "):
            captured_token = auth
            log.info("Token captured.")

    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=False)
        context = browser.new_context()
        page = context.new_page()
        page.on("request", on_request)
        page.goto("https://discord.com/login")

        print("\n  Log in to Discord in the browser window.")
        print("  It will close automatically once done.\n")

        timeout = 300
        start = time.time()
        try:
            while captured_token is None and (time.time() - start) < timeout:
                page.wait_for_timeout(1000)
                if "/channels" in page.url and captured_token is None:
                    page.wait_for_timeout(5000)
        except Exception:
            pass
        finally:
            browser.close()

    return captured_token
