#!/usr/bin/env bash
# Integration tests for signing up, signing in and deleting an account.
#
# These drive the real server over HTTP rather than calling into it, because
# everything worth checking here - the cookie flags, the redirects, the
# throttling - only exists at that level.
#
# The server's port is fixed at 8090, so this stops a running instance, works
# in a copy of the app under /tmp with its own database, and starts the real
# one again on the way out. The live database is never opened.
#
#   bash tests/auth_test.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SANDBOX=/tmp/auth_test.$$
BASE=http://127.0.0.1:8090
PASSED=0
FAILED=0

cleanup() {
    pkill -f "$SANDBOX/student_profiles" 2> /dev/null
    sleep 1
    # Belt and braces: nothing of this run may be left holding the port.
    pgrep -f "$SANDBOX" > /dev/null && pkill -9 -f "$SANDBOX" 2> /dev/null
    rm -rf "$SANDBOX" "$SANDBOX.png" "$SANDBOX.jar"
    [ -x "$ROOT/.devcontainer/start-server.sh" ] && bash "$ROOT/.devcontainer/start-server.sh" > /dev/null 2>&1
    echo
    echo "passed: $PASSED  failed: $FAILED"
    [ "$FAILED" -eq 0 ] || exit 1
}
trap cleanup EXIT

check() {
    local name="$1" want="$2" got="$3"

    if [ "$want" = "$got" ]; then
        PASSED=$((PASSED + 1))
        printf '  ok   %-52s %s\n' "$name" "$got"
    else
        FAILED=$((FAILED + 1))
        printf '  FAIL %-52s want=%s got=%s\n' "$name" "$want" "$got"
    fi
}

code() { curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$@"; }
body() { curl -s --max-time 20 "$@"; }

# ---------- a copy of the app with an empty database ----------

if [ ! -x "$ROOT/build/student_profiles" ]; then
    echo "build/student_profiles is missing - build it first." >&2
    exit 1
fi

pkill -f "[s]tudent_profiles" 2> /dev/null
sleep 1

mkdir -p "$SANDBOX/data" "$SANDBOX/static/uploads"
cp "$ROOT/build/student_profiles" "$SANDBOX/"
cp -r "$ROOT/templates" "$ROOT/database" "$SANDBOX/"
cp "$ROOT/static/style.css" "$SANDBOX/static/" 2> /dev/null

cd "$SANDBOX"
# Started by its full path on purpose. Launched as ./student_profiles the
# command line holds no trace of $SANDBOX, so the pkill in cleanup matched
# nothing, the test server outlived the run, and it went on answering on the
# port with the throwaway database while the real one never came back.
setsid nohup "$SANDBOX/student_profiles" > "$SANDBOX.log" 2>&1 < /dev/null &

for _ in $(seq 1 30); do
    curl -sf -o /dev/null --max-time 2 "$BASE/" && break
    sleep 1
done

printf '\x89PNG\r\n\x1a\n' > "$SANDBOX.png"
head -c 120 /dev/urandom >> "$SANDBOX.png"

# ---------- signing up ----------

echo "signing up"
check "new account is accepted" 302 "$(code -X POST \
    -F 'username=tester' -F 'password=CorrectPw1!' -F "photo=@$SANDBOX.png;type=image/png" \
    -F 'bio=hello' -F 'year=2' -F 'marks=70' -F 'program=BCA' "$BASE/signup")"

check "the same username is refused" 200 "$(code -X POST \
    -F 'username=tester' -F 'password=Another1!' -F "photo=@$SANDBOX.png;type=image/png" \
    -F 'bio=hello' -F 'year=2' -F 'marks=70' -F 'program=BCA' "$BASE/signup")"

check "photo is required" 200 "$(code -X POST \
    -F 'username=nophoto' -F 'password=CorrectPw1!' \
    -F 'bio=hello' -F 'year=2' -F 'marks=70' -F 'program=BCA' "$BASE/signup")"

check "the photo was written to disk" 1 "$(ls static/uploads | grep -c tester)"

# ---------- signing in ----------

echo "signing in"
check "the wrong password is refused" "yes" \
    "$(body -X POST -d 'username=tester&password=wrong' "$BASE/login" \
        | grep -qi 'Wrong username or password' && echo yes || echo no)"

check "a refused login sets no cookie" "no" \
    "$(curl -s -i --max-time 20 -X POST -d 'username=tester&password=wrong' "$BASE/login" \
        | grep -qi '^set-cookie' && echo yes || echo no)"

check "the right password signs you in" 302 \
    "$(code -c "$SANDBOX.jar" -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login")"

check "a session cookie is stored" "yes" \
    "$(grep -q session "$SANDBOX.jar" && echo yes || echo no)"

check "the cookie is HttpOnly and SameSite" "yes" \
    "$(curl -s -i --max-time 20 -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login" \
        | grep -i '^set-cookie' | grep -q 'HttpOnly.*SameSite=Strict' && echo yes || echo no)"

# Secure must appear only when the request arrived over https, or the cookie
# would stop coming back on plain http while the site is being worked on.
check "Secure is set behind an https proxy" "yes" \
    "$(curl -s -i --max-time 20 -H 'X-Forwarded-Proto: https' \
        -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login" \
        | grep -i '^set-cookie' | grep -q 'Secure' && echo yes || echo no)"

check "Secure is absent over plain http" "no" \
    "$(curl -s -i --max-time 20 -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login" \
        | grep -i '^set-cookie' | grep -q 'Secure' && echo yes || echo no)"

check "a signed in visitor reaches the profile" 200 "$(code -b "$SANDBOX.jar" "$BASE/profile")"
check "a signed out visitor is sent to login" 302 "$(code "$BASE/profile")"

# ---------- throttling ----------

echo "throttling"
# A different name each run, so the count starts from nothing.
VICTIM="target$$"
curl -s -o /dev/null --max-time 20 -X POST \
    -F "username=$VICTIM" -F 'password=CorrectPw1!' -F "photo=@$SANDBOX.png;type=image/png" \
    -F 'bio=x' -F 'year=1' -F 'marks=1' -F 'program=BCA' "$BASE/signup"

for _ in $(seq 1 8); do
    curl -s -o /dev/null --max-time 20 -X POST \
        -d "username=$VICTIM&password=wrong" "$BASE/login"
done

check "guessing is locked out after 8 tries" "yes" \
    "$(body -X POST -d "username=$VICTIM&password=wrong" "$BASE/login" \
        | grep -qi 'Too many attempts' && echo yes || echo no)"

# The lock has to hold even once the right password is offered, or anyone
# guessing would be told the moment they hit it.
check "the lock holds against the right password" "yes" \
    "$(body -X POST -d "username=$VICTIM&password=CorrectPw1!" "$BASE/login" \
        | grep -qi 'Too many attempts' && echo yes || echo no)"

check "an untouched account still signs in" 302 \
    "$(code -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login")"

# ---------- session lifetime ----------

echo "sessions"
if command -v sqlite3 > /dev/null; then
    sqlite3 data/students.db \
        "UPDATE sessions SET created_at = datetime('now', '-31 days');" 2> /dev/null
    check "a session past the cutoff stops working" 302 "$(code -b "$SANDBOX.jar" "$BASE/profile")"
else
    echo "  skip session expiry - sqlite3 is not installed"
fi

curl -s -o /dev/null -c "$SANDBOX.jar" --max-time 20 \
    -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login"
check "signing in again works after expiry" 200 "$(code -b "$SANDBOX.jar" "$BASE/profile")"

echo "signing out"
check "signing out redirects" 302 "$(code -b "$SANDBOX.jar" "$BASE/logout")"
check "the old cookie is dead afterwards" 302 "$(code -b "$SANDBOX.jar" "$BASE/profile")"

# ---------- deleting an account ----------

echo "deleting an account"
curl -s -o /dev/null -c "$SANDBOX.jar" --max-time 20 \
    -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login"

# Something of theirs has to exist before deleting means anything. A post
# with a file attached exercises both the rows and the file on disk.
curl -s -o /dev/null -b "$SANDBOX.jar" --max-time 20 \
    -F 'body=a post that should not outlive its author' \
    -F "file=@$SANDBOX.png;type=image/png" "$BASE/post"

check "the post shows in the feed first" 1 \
    "$(body "$BASE/" | grep -c 'should not outlive')"

check "a signed out visitor cannot delete" 302 \
    "$(code -X POST -d 'password=CorrectPw1!' "$BASE/account/delete")"

check "the wrong password deletes nothing" "yes" \
    "$(body -b "$SANDBOX.jar" -X POST -d 'password=wrong' "$BASE/account/delete" \
        | grep -qi 'not right' && echo yes || echo no)"

check "the account survived that" 200 "$(code -b "$SANDBOX.jar" "$BASE/profile")"

check "the right password deletes" 302 \
    "$(code -b "$SANDBOX.jar" -X POST -d 'password=CorrectPw1!' "$BASE/account/delete")"

check "the deleted account cannot sign in" "yes" \
    "$(body -X POST -d 'username=tester&password=CorrectPw1!' "$BASE/login" \
        | grep -qi 'Wrong username or password' && echo yes || echo no)"

check "their post is gone from the feed" 0 \
    "$(body "$BASE/" | grep -c 'should not outlive')"

check "their profile page is gone" 404 "$(code "$BASE/student/1")"

# Both the profile picture and the file attached to the post were named after
# them, so nothing of theirs should be left in the uploads folder.
check "their uploaded files are gone from disk" 0 "$(ls static/uploads | grep -c tester)"

check "search no longer finds them" 0 \
    "$(body "$BASE/search?q=tester" | grep -c '>tester<')"

check "the site still works afterwards" 200 "$(code "$BASE/")"
