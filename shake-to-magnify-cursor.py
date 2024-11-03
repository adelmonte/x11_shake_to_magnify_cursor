import sys
from PyQt5.QtWidgets import QApplication, QWidget
from PyQt5.QtCore import Qt, QTimer, QPoint, QSize
from PyQt5.QtGui import QPainter, QCursor, QPixmap
import math
import subprocess
import tempfile
import os
import time

class CursorScaler(QWidget):
    def __init__(self):
        super().__init__()
        # Window setup
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint | Qt.X11BypassWindowManagerHint)
        self.setAttribute(Qt.WA_TranslucentBackground)
        
        # Configuration
        self.min_scale = 1.0
        self.max_scale = 10.0
        self.current_scale = self.min_scale
        self.target_scale = self.min_scale
        self.scale_step = 0.1
        self.shake_threshold = 8
        self.shake_timeout = 0.3
        self.movement_threshold = 5

        # Shake detection state
        self.last_pos = QCursor().pos()
        self.last_direction = None
        self.direction_changes = 0
        self.last_change_time = time.time()
        self.last_movement_time = time.time()
        self.is_active = False
        self.is_scaling = False
        
        # Load cursor image
        self.cursor_pixmap = self.load_cursor_pixmap()
        self.base_size = max(32, self.cursor_pixmap.width())
        
        # Initialize widget size and hide by default
        initial_size = int(self.base_size * self.max_scale)
        self.resize(initial_size, initial_size)
        self.hide()
        
        # Adaptive timer with power-saving states
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.update_cursor)
        self.idle_interval = 250
        self.active_interval = 50
        self.very_idle_interval = 500
        self.idle_threshold = 2.0
        self.timer.start(self.idle_interval)

    def load_cursor_pixmap(self):
        if hasattr(self, '_cached_cursor'):
            return self._cached_cursor
            
        with tempfile.TemporaryDirectory() as tmp_dir:
            try:
                # Save current working directory
                original_cwd = os.getcwd()
                
                # Change to temporary directory for xcur2png execution
                os.chdir(tmp_dir)
                
                cursor_path = "/usr/share/icons/Simp1e-Dark/cursors/left_ptr"
                subprocess.run(['xcur2png', cursor_path], check=True)
                
                # Change back to original directory
                os.chdir(original_cwd)
                
                png_files = [f for f in os.listdir(tmp_dir) if f.endswith('.png')]
                largest_png = max(png_files, key=lambda f: os.path.getsize(os.path.join(tmp_dir, f)))
                self._cached_cursor = QPixmap(os.path.join(tmp_dir, largest_png))
                return self._cached_cursor
            except Exception:
                self._cached_cursor = self.create_fallback_cursor()
                return self._cached_cursor

    def create_fallback_cursor(self):
        pixmap = QPixmap(32, 32)
        pixmap.fill(Qt.transparent)
        painter = QPainter(pixmap)
        painter.setPen(Qt.black)
        painter.setBrush(Qt.black)
        painter.drawPolygon([QPoint(0, 0), QPoint(16, 16), QPoint(0, 32)])
        painter.setPen(Qt.white)
        painter.setBrush(Qt.white)
        painter.drawPolygon([QPoint(1, 1), QPoint(14, 14), QPoint(1, 29)])
        painter.end()
        return pixmap

    def get_movement_direction(self, delta):
        if abs(delta.x()) <= self.movement_threshold and abs(delta.y()) <= self.movement_threshold:
            return None
            
        if abs(delta.x()) > abs(delta.y()):
            return 'right' if delta.x() > 0 else 'left'
        elif abs(delta.y()) > abs(delta.x()):
            return 'down' if delta.y() > 0 else 'up'
        return None

    def update_scale(self):
        if not self.is_scaling:
            self.current_scale = max(self.min_scale, self.current_scale - 1.0)
            if self.current_scale <= self.min_scale:
                self.hide()
                return

        if self.is_scaling and self.current_scale < self.target_scale:
            self.current_scale = min(self.target_scale, self.current_scale + self.scale_step)
            if not self.isVisible():
                self.show()

    def update_cursor(self):
        current_time = time.time()
        current_pos = QCursor().pos()
        delta = current_pos - self.last_pos
        distance = math.hypot(delta.x(), delta.y())

        if distance > self.movement_threshold:
            self.last_movement_time = current_time
            if not self.is_active:
                self.is_active = True
                self.timer.setInterval(self.active_interval)
        elif current_time - self.last_movement_time > self.idle_threshold:
            if self.is_active or self.timer.interval() == self.idle_interval:
                self.is_active = False
                self.timer.setInterval(self.very_idle_interval)
        elif not self.is_active and self.timer.interval() != self.idle_interval:
            self.timer.setInterval(self.idle_interval)

        if distance > self.movement_threshold:
            current_direction = self.get_movement_direction(delta)
            
            if current_direction and current_direction != self.last_direction:
                if current_time - self.last_change_time < self.shake_timeout:
                    self.direction_changes += 1
                else:
                    self.direction_changes = 1
                self.last_change_time = current_time
            self.last_direction = current_direction

        if current_time - self.last_change_time > self.shake_timeout:
            self.direction_changes = 0

        if self.direction_changes >= self.shake_threshold:
            if not self.is_scaling:
                self.is_scaling = True
                self.current_scale = self.min_scale
                self.target_scale = self.min_scale + 0.5
            else:
                self.target_scale = min(self.max_scale, self.target_scale + 0.02)
        else:
            self.is_scaling = False
            self.target_scale = self.min_scale

        self.update_scale()
        
        if self.isVisible():
            scaled_size = int(self.base_size * self.current_scale)
            self.move(current_pos.x() - scaled_size // 4, current_pos.y() - scaled_size // 4)
            self.resize(scaled_size, scaled_size)
            self.update()

        self.last_pos = current_pos

    def paintEvent(self, event):
        if not self.isVisible():
            return
        painter = QPainter(self)
        current_size = int(self.base_size * self.current_scale)
        painter.drawPixmap(0, 0, current_size, current_size, self.cursor_pixmap)

def main():
    app = QApplication(sys.argv)
    app.setOverrideCursor(QCursor(Qt.BlankCursor))
    scaler = CursorScaler()
    app.aboutToQuit.connect(lambda: app.restoreOverrideCursor())
    sys.exit(app.exec_())

if __name__ == '__main__':
    main()