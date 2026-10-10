-- MedEvac schema for the current C++ server.
-- This file intentionally creates tables only. On an empty database, server.cpp seeds
-- its demo Pune road graph, ambulances and hospitals when the roads table is empty.
-- Do not run this over an existing database without taking a backup first.

CREATE TABLE IF NOT EXISTS roads (
    source TEXT NOT NULL,
    target TEXT NOT NULL,
    travel_time_mins INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS ambulances (
    id TEXT PRIMARY KEY,
    location TEXT NOT NULL,
    is_available INTEGER NOT NULL DEFAULT 1,
    type TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS hospitals (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    location TEXT NOT NULL,
    specialties TEXT NOT NULL DEFAULT '',
    available_beds INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS emergency_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    patient_name TEXT,
    severity INTEGER,
    specialty TEXT,
    location TEXT,
    assigned_ambulance TEXT,
    assigned_hospital TEXT
);

CREATE TABLE IF NOT EXISTS organization_registrations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    role TEXT NOT NULL,
    organization_name TEXT NOT NULL,
    location TEXT NOT NULL,
    specialties TEXT DEFAULT '',
    available_beds INTEGER DEFAULT 0,
    ambulance_type TEXT DEFAULT 'BLS',
    status TEXT NOT NULL DEFAULT 'PENDING',
    api_key TEXT DEFAULT '',
    resource_id TEXT DEFAULT '',
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);
