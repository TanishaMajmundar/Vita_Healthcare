const mongoose = require('mongoose');

const alertSchema = new mongoose.Schema({
  userId: { type: mongoose.Schema.Types.ObjectId, ref: 'User', required: true },
  type: { type: String, enum: ['fall', 'low_spo2', 'high_bpm', 'low_bpm'], default: 'fall' },
  message: { type: String, required: true },
  bpm: { type: Number, default: null },
  spo2: { type: Number, default: null },
  emailSent: { type: Boolean, default: false },
  resolved: { type: Boolean, default: false },
  resolvedAt: { type: Date, default: null },
  timestamp: { type: Date, default: Date.now }
});

alertSchema.index({ userId: 1, timestamp: -1 });

module.exports = mongoose.model('Alert', alertSchema);
