// common.js — shared auth helpers used by all dashboard pages

function checkAuth() {
  const token = localStorage.getItem('token');
  if (!token) window.location.href = 'login.html';
}

function loadUser() {
  const user = JSON.parse(localStorage.getItem('user') || '{}');
  const pill = document.getElementById('userPill');
  if (pill && user.name) pill.textContent = '👤 ' + user.name;
}

function logout() {
  localStorage.removeItem('token');
  localStorage.removeItem('user');
  window.location.href = 'login.html';
}
