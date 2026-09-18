CREATE TABLE IF NOT EXISTS students (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    password TEXT NOT NULL,
    bio TEXT NOT NULL,
    photo TEXT NOT NULL,
    year TEXT NOT NULL,
    program TEXT NOT NULL,
    marks TEXT NOT NULL,
    is_private INTEGER NOT NULL DEFAULT 0,
    dm_open INTEGER NOT NULL DEFAULT 1
);

-- Any student document: a marksheet, a certificate, a transcript, anything.
CREATE TABLE IF NOT EXISTS documents (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    student_id INTEGER NOT NULL,
    title TEXT NOT NULL,
    file_path TEXT NOT NULL,
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- Short updates a student writes, with an optional file attached.
CREATE TABLE IF NOT EXISTS posts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    student_id INTEGER NOT NULL,
    body TEXT NOT NULL,
    file_path TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- One row per direct message.
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id INTEGER NOT NULL,
    receiver_id INTEGER NOT NULL,
    body TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (sender_id) REFERENCES students(id),
    FOREIGN KEY (receiver_id) REFERENCES students(id)
);

CREATE TABLE IF NOT EXISTS follows (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    follower_id INTEGER NOT NULL,
    following_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    UNIQUE (follower_id, following_id),
    FOREIGN KEY (follower_id) REFERENCES students(id),
    FOREIGN KEY (following_id) REFERENCES students(id)
);

-- A login gives you a long random token. The token is what the cookie holds,
-- so a cookie cannot simply be edited to become another student.
CREATE TABLE IF NOT EXISTS sessions (
    token TEXT PRIMARY KEY,
    student_id INTEGER NOT NULL,
    -- When the login happened. A session older than the cutoff in main.cpp
    -- stops working, so a cookie that leaks is not useful forever.
    created_at TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- What a linked page says about itself, so a post can show a preview card.
-- Saved once per address so the same page is not fetched again and again.
CREATE TABLE IF NOT EXISTS link_previews (
    url TEXT PRIMARY KEY,
    title TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    image TEXT NOT NULL DEFAULT '',
    site TEXT NOT NULL DEFAULT ''
);

-- One row per student per post. The UNIQUE pair is what stops a student
-- liking the same post twice.
CREATE TABLE IF NOT EXISTS likes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    post_id INTEGER NOT NULL,
    student_id INTEGER NOT NULL,
    UNIQUE (post_id, student_id),
    FOREIGN KEY (post_id) REFERENCES posts(id),
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- A reply written under a post. The student who wrote the comment can take
-- it back, and so can the student whose post it sits under.
CREATE TABLE IF NOT EXISTS comments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    post_id INTEGER NOT NULL,
    student_id INTEGER NOT NULL,
    body TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (post_id) REFERENCES posts(id),
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- "somebody liked your post", "somebody commented on it". One row per event,
-- for the student being told. You are never told about your own doing.
CREATE TABLE IF NOT EXISTS notifications (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    student_id INTEGER NOT NULL,
    actor_id INTEGER NOT NULL,
    post_id INTEGER NOT NULL,
    kind TEXT NOT NULL,
    seen INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (student_id) REFERENCES students(id),
    FOREIGN KEY (actor_id) REFERENCES students(id),
    FOREIGN KEY (post_id) REFERENCES posts(id)
);

-- Without these every one of these lookups reads the whole table. The UNIQUE
-- pairs on follows and likes are already indexes, so they are not repeated.
CREATE INDEX IF NOT EXISTS idx_posts_student ON posts(student_id);
CREATE INDEX IF NOT EXISTS idx_comments_post ON comments(post_id);
CREATE INDEX IF NOT EXISTS idx_documents_student ON documents(student_id);
CREATE INDEX IF NOT EXISTS idx_messages_pair ON messages(sender_id, receiver_id);
CREATE INDEX IF NOT EXISTS idx_notifications_owner ON notifications(student_id, seen);
