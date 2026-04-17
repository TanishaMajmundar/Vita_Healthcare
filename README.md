# 🚑 IoT Health Monitor — MERN Stack

Real-time IoT-based health monitoring system with fall detection, built on the MERN stack (MongoDB, Express, React-free Node.js, HTML/CSS/JS frontend).

---

## 🗂 Project Structure

```
iot-health-mern/
├── backend/
│   ├── models/
│   │   ├── User.js          # User schema (bcrypt password)
│   │   ├── HealthData.js    # Sensor readings schema
│   │   └── Alert.js         # Fall & health alerts schema
│   ├── routes/
│   │   ├── auth.js          # Register, Login, Profile CRUD
│   │   ├── health.js        # IoT POST + full CRUD on readings
│   │   ├── alerts.js        # Alert management CRUD
│   │   └── users.js         # Admin user management
│   ├── middleware/
│   │   └── auth.js          # JWT verification middleware
│   ├── .env                 # Environment variables
│   └── server.js            # Express entry point
├── frontend/
│   ├── pages/
│   │   ├── login.html       # Auth - login
│   │   ├── register.html    # Auth - register
│   │   ├── dashboard.html   # Live vitals + charts
│   │   ├── history.html     # Paginated health history
│   │   ├── alerts.html      # Alert management
│   │   └── profile.html     # User profile & device settings
│   └── public/
│       └── css/
│           ├── styles.css   # Shared styles
│           └── common.js    # Auth helpers (checkAuth, logout)
└── postman/
    └── IoT_Health_Monitor.postman_collection.json
```

---

## ⚙️ Setup & Run

### Prerequisites
- Node.js v18+
- MongoDB running locally (`mongod`) or MongoDB Atlas URI

### 1. Backend

```bash
cd backend
npm install
# Edit .env — set MONGO_URI, JWT_SECRET, MAIL_USER, MAIL_PASS
node server.js
# Server starts on http://localhost:5000
```

### 2. Frontend

Open `frontend/pages/login.html` in your browser (use Live Server in VS Code for best results).

### 3. ESP8266 Arduino

No change needed — the device still POSTs to `/api/health/data` with `{ bpm, spo2, fall, deviceId }`.  
Make sure the `deviceId` in your `.ino` matches the `deviceId` set in the user's profile.

---

## 🔗 API Endpoints

### Auth
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| POST | /api/auth/register | ❌ | Register new user |
| POST | /api/auth/login | ❌ | Login, get JWT token |
| GET | /api/auth/me | ✅ | Get own profile |
| PUT | /api/auth/profile | ✅ | Update profile |
| DELETE | /api/auth/account | ✅ | Delete own account |

### Health Data
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| POST | /api/health/data | ❌ | IoT device sends readings |
| GET | /api/health/live | ✅ | Latest reading |
| GET | /api/health/history | ✅ | Last 20 readings |
| GET | /api/health/all | ✅ | Paginated full history |
| GET | /api/health/:id | ✅ | Single record |
| PUT | /api/health/:id | ✅ | Update notes on record |
| DELETE | /api/health/:id | ✅ | Delete one record |
| DELETE | /api/health/ | ✅ | Clear all records |

### Alerts
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| GET | /api/alerts | ✅ | All alerts (paginated) |
| GET | /api/alerts/unresolved/count | ✅ | Badge count |
| GET | /api/alerts/:id | ✅ | Single alert |
| PUT | /api/alerts/:id/resolve | ✅ | Mark resolved |
| DELETE | /api/alerts/:id | ✅ | Delete alert |
| DELETE | /api/alerts/ | ✅ | Clear resolved alerts |

### Users (Admin)
| Method | Endpoint | Auth | Description |
|--------|----------|------|-------------|
| GET | /api/users | ✅ Admin | List all users |
| GET | /api/users/:id | ✅ | Get user |
| PUT | /api/users/:id | ✅ | Update user |
| DELETE | /api/users/:id | ✅ Admin | Delete user |

---

## 🧪 Testing with Postman

1. Import `postman/IoT_Health_Monitor.postman_collection.json` into Postman
2. Run **Register User** — token is auto-saved to collection variable
3. Use any other request — token is sent automatically via `{{token}}`

---

## 📦 Tech Stack

| Layer | Technology |
|-------|-----------|
| Database | MongoDB + Mongoose |
| Backend | Node.js + Express |
| Auth | JWT + bcryptjs |
| Email | Nodemailer + Gmail SMTP |
| Frontend | HTML5 + CSS3 + Vanilla JS |
| Charts | Chart.js |
| IoT Device | ESP8266 + MAX30102 + MPU6050 |
