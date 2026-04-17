const mongoose = require('mongoose');

const healthDataSchema = new mongoose.Schema({
  userId: { type: mongoose.Schema.Types.ObjectId, ref: 'User', required: true },
  deviceId: { type: String, default: 'ESP8266' },
  bpm: { type: Number, default: null },
  spo2: { type: Number, default: null },
  fall: { type: Boolean, default: false },
  status: { type: String, enum: ['monitoring', 'countdown', 'alert_sent', 'safe'], default: 'monitoring' },
  notes: { type: String, default: '' },
  timestamp: { type: Date, default: Date.now }
});

// Index for fast time-range queries
healthDataSchema.index({ userId: 1, timestamp: -1 });

module.exports = mongoose.model('HealthData', healthDataSchema);
