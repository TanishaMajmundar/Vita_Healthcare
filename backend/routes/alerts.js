const express = require('express');
const router = express.Router();
const Alert = require('../models/Alert');
const auth = require('../middleware/auth');

// GET /api/alerts  — all alerts for user
router.get('/', auth, async (req, res) => {
  try {
    const page = parseInt(req.query.page) || 1;
    const limit = parseInt(req.query.limit) || 20;
    const skip = (page - 1) * limit;
    const filter = { userId: req.user._id };
    if (req.query.resolved !== undefined) filter.resolved = req.query.resolved === 'true';

    const total = await Alert.countDocuments(filter);
    const alerts = await Alert.find(filter).sort({ timestamp: -1 }).skip(skip).limit(limit);
    res.json({ total, page, pages: Math.ceil(total / limit), data: alerts });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/alerts/unresolved/count
router.get('/unresolved/count', auth, async (req, res) => {
  try {
    const count = await Alert.countDocuments({ userId: req.user._id, resolved: false });
    res.json({ count });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/alerts/:id
router.get('/:id', auth, async (req, res) => {
  try {
    const alert = await Alert.findOne({ _id: req.params.id, userId: req.user._id });
    if (!alert) return res.status(404).json({ error: 'Alert not found' });
    res.json(alert);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// PUT /api/alerts/:id/resolve
router.put('/:id/resolve', auth, async (req, res) => {
  try {
    const alert = await Alert.findOneAndUpdate(
      { _id: req.params.id, userId: req.user._id },
      { resolved: true, resolvedAt: new Date() },
      { new: true }
    );
    if (!alert) return res.status(404).json({ error: 'Alert not found' });
    res.json({ message: 'Alert resolved', alert });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// DELETE /api/alerts/:id
router.delete('/:id', auth, async (req, res) => {
  try {
    const deleted = await Alert.findOneAndDelete({ _id: req.params.id, userId: req.user._id });
    if (!deleted) return res.status(404).json({ error: 'Alert not found' });
    res.json({ message: 'Alert deleted' });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// DELETE /api/alerts/  — clear all resolved
router.delete('/', auth, async (req, res) => {
  try {
    const result = await Alert.deleteMany({ userId: req.user._id, resolved: true });
    res.json({ message: `Deleted ${result.deletedCount} resolved alerts` });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

module.exports = router;
