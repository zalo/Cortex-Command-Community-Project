/**
 * Touch gamepad overlay for Cortex Command on mobile/tablet.
 *
 * Maps to CC's PresetWASDKeys (keyboard-only) input scheme:
 *
 * Left stick:  Movement (WASD) + Aim direction (W/S also aim up/down)
 * Right side:  Action buttons mapped to keyboard keys
 *
 * No mouse events — pure keyboard input for clean touch control.
 */
(function() {
  'use strict';

  if (!('ontouchstart' in window) && navigator.maxTouchPoints <= 0) return;

  var canvas = null;
  var audioUnlocked = false;

  // =========================================================================
  // Audio unlock
  // =========================================================================
  function unlockAudio() {
    if (audioUnlocked) return;
    audioUnlocked = true;
    if (window._ccAudioCtx && window._ccAudioCtx.state === 'suspended') {
      window._ccAudioCtx.resume();
    }
    window._ccAudioUnlocked = true;
    if (window._ccAudioQueue) {
      window._ccAudioQueue.forEach(function(fn) { try { fn(); } catch(e) {} });
      window._ccAudioQueue = [];
    }
    try {
      if (typeof Module !== 'undefined' && Module.SDL3 && Module.SDL3.audioContext) {
        Module.SDL3.audioContext.resume();
      }
    } catch(e) {}
  }

  // =========================================================================
  // Key dispatch
  // =========================================================================
  var heldKeys = {};

  function pressKey(code, key, keyCode) {
    if (heldKeys[code]) return;
    heldKeys[code] = true;
    canvas.dispatchEvent(new KeyboardEvent('keydown', {
      code: code, key: key, keyCode: keyCode,
      bubbles: true, cancelable: true
    }));
  }

  function releaseKey(code, key, keyCode) {
    if (!heldKeys[code]) return;
    heldKeys[code] = false;
    canvas.dispatchEvent(new KeyboardEvent('keyup', {
      code: code, key: key, keyCode: keyCode,
      bubbles: true, cancelable: true
    }));
  }

  // =========================================================================
  // PresetWASDKeys keybindings
  // [code, key, keyCode]
  // =========================================================================
  var K = {
    // Movement + Aim
    W:      ['KeyW',        'w',       87],  // Move/Aim Up
    A:      ['KeyA',        'a',       65],  // Move Left
    S:      ['KeyS',        's',       83],  // Move/Aim Down
    D:      ['KeyD',        'd',       68],  // Move Right
    // Actions
    H:      ['KeyH',        'h',       72],  // Fire
    J:      ['KeyJ',        'j',       74],  // Sharp Aim (hold)
    K:      ['KeyK',        'k',       75],  // Pie Menu
    L:      ['KeyL',        'l',       76],  // Jump
    LSHIFT: ['ShiftLeft',   'Shift',   16],  // Sprint
    LCTRL:  ['ControlLeft', 'Control', 17],  // Crouch
    C:      ['KeyC',        'c',       67],  // Prone
    // Weapons
    R:      ['KeyR',        'r',       82],  // Reload
    F:      ['KeyF',        'f',       70],  // Pick Up
    B:      ['KeyB',        'b',       66],  // Drop
    Q:      ['KeyQ',        'q',       81],  // Prev Weapon
    E:      ['KeyE',        'e',       69],  // Next Weapon
    // Bodies
    Y:      ['KeyY',        'y',       89],  // Prev Body
    U:      ['KeyU',        'u',       85],  // Next Body
    // Menu
    ESC:    ['Escape',      'Escape',  27],
  };

  // =========================================================================
  // Build the gamepad UI
  // =========================================================================
  function createGamepad() {
    var gp = document.createElement('div');
    gp.id = 'touch-gamepad';
    gp.innerHTML = [
      '<style>',
      '#touch-gamepad {',
      '  position: fixed; top: 0; left: 0; width: 100%; height: 100%;',
      '  z-index: 10000; pointer-events: none;',
      '  user-select: none; -webkit-user-select: none;',
      '}',
      // --- Analog stick ---
      '#tp-stick-zone {',
      '  position: absolute; left: 8px; bottom: 8px;',
      '  width: 170px; height: 170px;',
      '  pointer-events: auto; touch-action: none;',
      '}',
      '#tp-stick-base {',
      '  position: absolute; left: 15px; bottom: 15px;',
      '  width: 140px; height: 140px; border-radius: 50%;',
      '  background: rgba(255,255,255,0.10);',
      '  border: 2px solid rgba(255,255,255,0.20);',
      '}',
      '#tp-stick-knob {',
      '  position: absolute; width: 54px; height: 54px; border-radius: 50%;',
      '  background: rgba(255,255,255,0.35);',
      '  left: 50%; top: 50%; transform: translate(-50%, -50%);',
      '  pointer-events: none;',
      '}',
      // --- Round action buttons ---
      '.tp-btn {',
      '  position: absolute; pointer-events: auto; touch-action: none;',
      '  border-radius: 50%; display: flex; align-items: center; justify-content: center;',
      '  font-family: monospace; font-weight: bold;',
      '  color: rgba(255,255,255,0.7);',
      '  border: 2px solid rgba(255,255,255,0.20);',
      '  transition: background 0.05s;',
      '}',
      '.tp-btn.active { background: rgba(255,255,255,0.35) !important; }',
      // --- Pill buttons ---
      '.tp-pill {',
      '  position: absolute; pointer-events: auto; touch-action: none;',
      '  border-radius: 16px; display: flex; align-items: center; justify-content: center;',
      '  font-family: monospace; font-size: 10px; font-weight: bold;',
      '  color: rgba(255,255,255,0.6);',
      '  border: 2px solid rgba(255,255,255,0.15);',
      '  background: rgba(255,255,255,0.06);',
      '  transition: background 0.05s;',
      '}',
      '.tp-pill.active { background: rgba(255,255,255,0.30) !important; }',

      // --- Right side: main actions (diamond layout like a gamepad) ---
      '#tp-fire {',                // Right = Fire (H)
      '  right: 15px; bottom: 95px; width: 68px; height: 68px;',
      '  background: rgba(255,60,60,0.25); font-size: 12px;',
      '}',
      '#tp-jump {',               // Top = Jump (L)
      '  right: 75px; bottom: 160px; width: 58px; height: 58px;',
      '  background: rgba(80,180,255,0.20); font-size: 11px;',
      '}',
      '#tp-crouch {',             // Bottom = Crouch (Ctrl)
      '  right: 75px; bottom: 30px; width: 54px; height: 54px;',
      '  background: rgba(80,255,80,0.15); font-size: 9px;',
      '}',
      '#tp-pie {',                // Left = Pie Menu (K)
      '  right: 140px; bottom: 95px; width: 54px; height: 54px;',
      '  background: rgba(255,200,80,0.18); font-size: 10px;',
      '}',
      '#tp-aim {',                // Center = Sharp Aim (J)
      '  right: 82px; bottom: 100px; width: 44px; height: 44px;',
      '  background: rgba(200,100,255,0.18); font-size: 8px;',
      '}',

      // --- Top bar: utility pills ---
      '#tp-sprint { left: 8px; top: 8px; width: 58px; height: 28px; }',
      '#tp-reload { right: 8px; top: 8px; width: 58px; height: 28px; }',
      '#tp-pickup { right: 74px; top: 8px; width: 58px; height: 28px; }',
      '#tp-drop   { right: 140px; top: 8px; width: 50px; height: 28px; }',
      '#tp-prev   { right: 58px; top: 44px; width: 42px; height: 28px; }',
      '#tp-next   { right: 8px; top: 44px; width: 42px; height: 28px; }',
      '#tp-menu   { left: 50%; top: 8px; transform: translateX(-50%); width: 48px; height: 26px; }',

      // --- Left side: extra pills ---
      '#tp-prone   { left: 8px; top: 44px; width: 55px; height: 28px; }',
      '#tp-prevbody { left: 74px; top: 8px; width: 28px; height: 28px; font-size: 9px; }',
      '#tp-nextbody { left: 108px; top: 8px; width: 28px; height: 28px; font-size: 9px; }',
      '</style>',

      // Analog stick
      '<div id="tp-stick-zone">',
      '  <div id="tp-stick-base"><div id="tp-stick-knob"></div></div>',
      '</div>',

      // Main action buttons (right side — diamond)
      '<div class="tp-btn" id="tp-fire">FIRE</div>',
      '<div class="tp-btn" id="tp-jump">JUMP</div>',
      '<div class="tp-btn" id="tp-crouch">DUCK</div>',
      '<div class="tp-btn" id="tp-pie">PIE</div>',
      '<div class="tp-btn" id="tp-aim">AIM</div>',

      // Top bar utility pills
      '<div class="tp-pill" id="tp-sprint">SPRINT</div>',
      '<div class="tp-pill" id="tp-reload">RELOAD</div>',
      '<div class="tp-pill" id="tp-pickup">PICK UP</div>',
      '<div class="tp-pill" id="tp-drop">DROP</div>',
      '<div class="tp-pill" id="tp-prev">W◀</div>',
      '<div class="tp-pill" id="tp-next">W▶</div>',
      '<div class="tp-pill" id="tp-menu">ESC</div>',

      // Left side extras
      '<div class="tp-pill" id="tp-prone">PRONE</div>',
      '<div class="tp-pill" id="tp-prevbody">◀</div>',
      '<div class="tp-pill" id="tp-nextbody">▶</div>',
    ].join('\n');

    document.body.appendChild(gp);

    // =======================================================================
    // Analog stick → WASD (movement + aim up/down)
    // =======================================================================
    // In PresetWASDKeys: W = move up + aim up, S = move down + aim down,
    // A = move left, D = move right. The character faces the direction of
    // horizontal movement and aims in the vertical direction.
    setupStick('tp-stick-zone', 'tp-stick-base', 'tp-stick-knob',
      function(dx, dy) {
        if (dx < -0.3) pressKey(K.A[0], K.A[1], K.A[2]);
        else            releaseKey(K.A[0], K.A[1], K.A[2]);
        if (dx >  0.3) pressKey(K.D[0], K.D[1], K.D[2]);
        else            releaseKey(K.D[0], K.D[1], K.D[2]);
        if (dy < -0.3) pressKey(K.W[0], K.W[1], K.W[2]);
        else            releaseKey(K.W[0], K.W[1], K.W[2]);
        if (dy >  0.3) pressKey(K.S[0], K.S[1], K.S[2]);
        else            releaseKey(K.S[0], K.S[1], K.S[2]);
      },
      function() {
        releaseKey(K.A[0], K.A[1], K.A[2]);
        releaseKey(K.D[0], K.D[1], K.D[2]);
        releaseKey(K.W[0], K.W[1], K.W[2]);
        releaseKey(K.S[0], K.S[1], K.S[2]);
      }
    );

    // =======================================================================
    // Action buttons
    // =======================================================================
    setupButton('tp-fire', function(down) {       // Fire = H
      unlockAudio();
      if (down) pressKey(K.H[0], K.H[1], K.H[2]);
      else      releaseKey(K.H[0], K.H[1], K.H[2]);
    });

    setupButton('tp-jump', function(down) {       // Jump = L
      unlockAudio();
      if (down) pressKey(K.L[0], K.L[1], K.L[2]);
      else      releaseKey(K.L[0], K.L[1], K.L[2]);
    });

    setupButton('tp-crouch', function(down) {     // Crouch = Left Ctrl
      if (down) pressKey(K.LCTRL[0], K.LCTRL[1], K.LCTRL[2]);
      else      releaseKey(K.LCTRL[0], K.LCTRL[1], K.LCTRL[2]);
    });

    setupButton('tp-pie', function(down) {        // Pie Menu = K
      unlockAudio();
      if (down) pressKey(K.K[0], K.K[1], K.K[2]);
      else      releaseKey(K.K[0], K.K[1], K.K[2]);
    });

    setupButton('tp-aim', function(down) {        // Sharp Aim = J
      if (down) pressKey(K.J[0], K.J[1], K.J[2]);
      else      releaseKey(K.J[0], K.J[1], K.J[2]);
    });

    // =======================================================================
    // Utility buttons
    // =======================================================================
    setupButton('tp-sprint', function(down) {     // Sprint = Left Shift
      if (down) pressKey(K.LSHIFT[0], K.LSHIFT[1], K.LSHIFT[2]);
      else      releaseKey(K.LSHIFT[0], K.LSHIFT[1], K.LSHIFT[2]);
    });

    setupButton('tp-reload', function(down) {     // Reload = R
      if (down) pressKey(K.R[0], K.R[1], K.R[2]);
      else      releaseKey(K.R[0], K.R[1], K.R[2]);
    });

    setupButton('tp-pickup', function(down) {     // Pick Up = F
      if (down) pressKey(K.F[0], K.F[1], K.F[2]);
      else      releaseKey(K.F[0], K.F[1], K.F[2]);
    });

    setupButton('tp-drop', function(down) {       // Drop = B
      if (down) pressKey(K.B[0], K.B[1], K.B[2]);
      else      releaseKey(K.B[0], K.B[1], K.B[2]);
    });

    setupButton('tp-prev', function(down) {       // Prev Weapon = Q
      if (down) pressKey(K.Q[0], K.Q[1], K.Q[2]);
      else      releaseKey(K.Q[0], K.Q[1], K.Q[2]);
    });

    setupButton('tp-next', function(down) {       // Next Weapon = E
      if (down) pressKey(K.E[0], K.E[1], K.E[2]);
      else      releaseKey(K.E[0], K.E[1], K.E[2]);
    });

    setupButton('tp-menu', function(down) {       // Menu = Escape
      if (down) pressKey(K.ESC[0], K.ESC[1], K.ESC[2]);
      else      releaseKey(K.ESC[0], K.ESC[1], K.ESC[2]);
    });

    setupButton('tp-prone', function(down) {      // Prone = C
      if (down) pressKey(K.C[0], K.C[1], K.C[2]);
      else      releaseKey(K.C[0], K.C[1], K.C[2]);
    });

    setupButton('tp-prevbody', function(down) {   // Prev Body = Y
      if (down) pressKey(K.Y[0], K.Y[1], K.Y[2]);
      else      releaseKey(K.Y[0], K.Y[1], K.Y[2]);
    });

    setupButton('tp-nextbody', function(down) {   // Next Body = U
      if (down) pressKey(K.U[0], K.U[1], K.U[2]);
      else      releaseKey(K.U[0], K.U[1], K.U[2]);
    });

    console.log('[TouchGamepad] Initialized — PresetWASDKeys (keyboard only)');
  }

  // =========================================================================
  // Analog stick handler
  // =========================================================================
  function setupStick(zoneId, baseId, knobId, onMove, onRelease) {
    var zone = document.getElementById(zoneId);
    var base = document.getElementById(baseId);
    var knob = document.getElementById(knobId);
    var touchId = null;
    var cx, cy, maxR;

    zone.addEventListener('touchstart', function(e) {
      e.preventDefault();
      if (touchId !== null) return;
      var t = e.changedTouches[0];
      touchId = t.identifier;
      var rect = base.getBoundingClientRect();
      cx = rect.left + rect.width / 2;
      cy = rect.top + rect.height / 2;
      maxR = rect.width / 2;
      update(t.clientX, t.clientY);
    }, {passive: false});

    zone.addEventListener('touchmove', function(e) {
      e.preventDefault();
      for (var i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === touchId) {
          update(e.changedTouches[i].clientX, e.changedTouches[i].clientY);
          break;
        }
      }
    }, {passive: false});

    function update(tx, ty) {
      var dx = tx - cx, dy = ty - cy;
      var dist = Math.sqrt(dx * dx + dy * dy);
      if (dist > maxR) { dx = dx / dist * maxR; dy = dy / dist * maxR; }
      knob.style.left = (50 + (dx / maxR) * 40) + '%';
      knob.style.top  = (50 + (dy / maxR) * 40) + '%';
      onMove(dx / maxR, dy / maxR);
    }

    function end(e) {
      for (var i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === touchId) {
          touchId = null;
          knob.style.left = '50%';
          knob.style.top  = '50%';
          onRelease();
          break;
        }
      }
    }
    zone.addEventListener('touchend', end, {passive: false});
    zone.addEventListener('touchcancel', end);
  }

  // =========================================================================
  // Button handler
  // =========================================================================
  function setupButton(id, callback) {
    var btn = document.getElementById(id);
    if (!btn) return;
    var activeTouchId = null;

    btn.addEventListener('touchstart', function(e) {
      e.preventDefault();
      if (activeTouchId !== null) return;
      activeTouchId = e.changedTouches[0].identifier;
      btn.classList.add('active');
      callback(true);
    }, {passive: false});

    btn.addEventListener('touchend', function(e) {
      e.preventDefault();
      for (var i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === activeTouchId) {
          activeTouchId = null;
          btn.classList.remove('active');
          callback(false);
          break;
        }
      }
    }, {passive: false});

    btn.addEventListener('touchcancel', function(e) {
      for (var i = 0; i < e.changedTouches.length; i++) {
        if (e.changedTouches[i].identifier === activeTouchId) {
          activeTouchId = null;
          btn.classList.remove('active');
          callback(false);
          break;
        }
      }
    });
  }

  // =========================================================================
  // Init
  // =========================================================================
  function init() {
    canvas = document.getElementById('canvas');
    if (!canvas) { setTimeout(init, 500); return; }
    createGamepad();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
