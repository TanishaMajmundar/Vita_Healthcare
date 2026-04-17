const express = require('express');
const router = express.Router();
const HealthData = require('../models/HealthData');
const Alert = require('../models/Alert');
const auth = require('../middleware/auth');
const nodemailer = require('nodemailer');

// Email transporter
const transporter = nodemailer.createTransport({
  service: 'gmail',
  auth: { user: process.env.MAIL_USER, pass: process.env.MAIL_PASS }
});

async function sendFallEmail(userId, bpm, spo2) {
  const recipients = process.env.ALERT_RECIPIENTS?.split(',') || [];
  if (!recipients.length) return;
  try {
    await transporter.sendMail({
      from: process.env.MAIL_USER,
      to: recipients.join(','),
      subject: '🚨 FALL ALERT - IoT Health Monitor',
      html: `<h2 style="color:red">⚠️ FALL DETECTED</h2>
             <p>The monitored user has fallen and did not respond to the alert.</p>
             <p><strong>BPM:</strong> ${bpm || '--'}</p>
             <p><strong>SpO₂:</strong> ${spo2 || '--'}%</p>
             <p><strong>Time:</strong> ${new Date().toLocaleString()}</p>
             <p>Please check immediately.</p>`
    });
  } catch (e) { console.error('Email error:', e.message); }
}

// POST /api/health/data  — called by ESP8266
router.post('/data', async (req, res) => {
  try {
    const { bpm, spo2, fall, status, deviceId } = req.body;

    // Find user by deviceId (IoT device doesn't send JWT)
    const User = require('../models/User');
    const user = await User.findOne({ deviceId: deviceId || 'ESP8266' });
    const userId = user ? user._id : null;

    if (!userId) {
      return res.status(404).json({ error: 'Device not registered to any user' });
    }

    // Validate ranges
    const validBpm = bpm >= 50 && bpm <= 160 ? bpm : null;
    const validSpo2 = spo2 >= 80 && spo2 <= 100 ? spo2 : null;

    const record = await HealthData.create({ userId, deviceId, bpm: validBpm, spo2: validSpo2, fall, status });

    // Create and send fall alert
    if (fall) {
      const alert = await Alert.create({
        userId,
        type: 'fall',
        message: `Fall detected. BPM: ${validBpm}, SpO2: ${validSpo2}%`,
        bpm: validBpm,
        spo2: validSpo2
      });
      sendFallEmail(userId, validBpm, validSpo2).then(() => {
        Alert.findByIdAndUpdate(alert._id, { emailSent: true }).exec();
      });
    }

    // Low SpO2 alert
    if (validSpo2 && validSpo2 < 90) {
      await Alert.create({
        userId, type: 'low_spo2',
        message: `Low SpO₂: ${validSpo2}%`, bpm: validBpm, spo2: validSpo2
      });
    }

    res.json({ status: 'received', id: record._id });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/health/live  — latest reading for logged-in user
router.get('/live', auth, async (req, res) => {
  try {
    const latest = await HealthData.findOne({ userId: req.user._id }).sort({ timestamp: -1 });
    res.json(latest || {});
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/health/history  — last N records
router.get('/history', auth, async (req, res) => {
  try {
    const limit = parseInt(req.query.limit) || 20;
    const records = await HealthData.find({ userId: req.user._id })
      .sort({ timestamp: -1 })
      .limit(limit);
    res.json(records.reverse());
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/health/all  — paginated full history
router.get('/all', auth, async (req, res) => {
  try {
    const page = parseInt(req.query.page) || 1;
    const limit = parseInt(req.query.limit) || 50;
    const skip = (page - 1) * limit;

    const total = await HealthData.countDocuments({ userId: req.user._id });
    const records = await HealthData.find({ userId: req.user._id })
      .sort({ timestamp: -1 })
      .skip(skip)
      .limit(limit);

    res.json({ total, page, pages: Math.ceil(total / limit), data: records });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/health/:id  — single record
router.get('/:id', auth, async (req, res) => {
  try {
    const record = await HealthData.findOne({ _id: req.params.id, userId: req.user._id });
    if (!record) return res.status(404).json({ error: 'Record not found' });
    res.json(record);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// PUT /api/health/:id  — update notes on a record
router.put('/:id', auth, async (req, res) => {
  try {
    const { notes } = req.body;
    const updated = await HealthData.findOneAndUpdate(
      { _id: req.params.id, userId: req.user._id },
      { notes },
      { new: true }
    );
    if (!updated) return res.status(404).json({ error: 'Record not found' });
    res.json({ message: 'Updated', data: updated });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// DELETE /api/health/:id  — delete a record
router.delete('/:id', auth, async (req, res) => {
  try {
    const deleted = await HealthData.findOneAndDelete({ _id: req.params.id, userId: req.user._id });
    if (!deleted) return res.status(404).json({ error: 'Record not found' });
    res.json({ message: 'Record deleted' });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// DELETE /api/health/  — clear all records for user
router.delete('/', auth, async (req, res) => {
  try {
    const result = await HealthData.deleteMany({ userId: req.user._id });
    res.json({ message: `Deleted ${result.deletedCount} records` });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

module.exports = router;
