# Complete code
import tkinter as tk
from tkinter import ttk

class Settings:
    def __init__(self, root):
        self.root = root
        self.root.title("AetherSDR Settings")
        self.root.geometry("300x200")

        self.update_check_var = tk.BooleanVar()
        self.update_check_var.set(True)

        self.update_check_frame = ttk.Frame(self.root)
        self.update_check_frame.pack(padx=10, pady=10)

        self.update_check_label = ttk.Label(self.update_check_frame, text="Proactive Update Check:")
        self.update_check_label.pack(side=tk.LEFT)

        self.update_check_checkbox = ttk.Checkbutton(self.update_check_frame, variable=self.update_check_var)
        self.update_check_checkbox.pack(side=tk.LEFT)

    def get_update_check_status(self):
        return self.update_check_var.get()