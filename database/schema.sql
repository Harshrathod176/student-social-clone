CREATE TABLE IF NOT EXISTS students (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    password TEXT NOT NULL,
    bio TEXT NOT NULL,
    photo TEXT NOT NULL,
    year TEXT NOT NULL,
    program TEXT NOT NULL,
    marks TEXT NOT NULL,
    is_private INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS certificates (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    student_id INTEGER NOT NULL,
    title TEXT NOT NULL,
    file_path TEXT NOT NULL,
    FOREIGN KEY (student_id) REFERENCES students(id)
);

-- One row per follow. status is 'pending' while the other student has not
-- accepted yet, and 'accepted' once they have.
CREATE TABLE IF NOT EXISTS follows (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    follower_id INTEGER NOT NULL,
    following_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    UNIQUE (follower_id, following_id),
    FOREIGN KEY (follower_id) REFERENCES students(id),
    FOREIGN KEY (following_id) REFERENCES students(id)
);
