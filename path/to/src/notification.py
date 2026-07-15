# Complete code
import tkinter as tk
from tkinter import messagebox

class Notification:
    def show_message(self, title, message):
        messagebox.showinfo(title, message)