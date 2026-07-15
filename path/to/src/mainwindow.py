# Complete code
import tkinter as tk
from tkinter import ttk
from src.settings import Settings
from src.update_checker import UpdateChecker
from src.notification import Notification

class MainWindow:
    def __init__(self, root):
        self.root = root
        self.root.title("AetherSDR")
        self.root.geometry("800x600")

        self.settings = Settings(self.root)
        self.update_checker = UpdateChecker(self.settings)
        self.notification = Notification()

        self.update_check_button = ttk.Button(self.root, text="Update Check", command=self.update_check)
        self.update_check_button.pack(padx=10, pady=10)

        self.update_status_label = ttk.Label(self.root, text="Update Status:")
        self.update_status_label.pack(padx=10, pady=10)

        self.update_checker.run()

    def update_check(self):
        self.update_checker.check_for_updates()