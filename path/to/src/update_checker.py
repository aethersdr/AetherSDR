# Complete code
import requests
import tkinter as tk
from tkinter import messagebox
import threading
import time

class UpdateChecker:
    def __init__(self, settings):
        self.settings = settings
        self.update_available = False

    def check_for_updates(self):
        try:
            response = requests.get("https://example.com/update-check")
            if response.status_code == 200:
                self.update_available = response.json()["update_available"]
                if self.update_available:
                    messagebox.showinfo("Update Available", "A new update is available.")
        except requests.exceptions.RequestException as e:
            messagebox.showerror("Error", "Failed to check for updates: " + str(e))

    def start_update_check(self):
        if self.settings.get_update_check_status():
            threading.Thread(target=self.check_for_updates).start()
            threading.Timer(3600, self.start_update_check).start()  # Check every hour

    def run(self):
        self.start_update_check()