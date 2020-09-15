# Student code that plays pitches from keyboard inputs
import time

SOUND = '59_1'

################################# GLOBAL VARS ##################################

NOTES = 'CC GG AA G  FF EE DD C'
PAUSE_DURATION = 0.125  # The duration (s) to wait during a whitespace
NOTE_DURATION = 0.75    # The duration that a note is held before playing the next

# Mapping notes to pitches
# https://www.szynalski.com/tone-generator/
MAP = {
    'C' : 261,
    'D' : 294,
    'E' : 330,
    'F' : 349,
    'G' : 392,
    'A' : 440,
    'B' : 494
}

################################## AUTONOMOUS ##################################

def autonomous_setup():
    print("Now executing AUTONOMOUS SETUP")
    # Write pitches
    for note in NOTES:
        if (note == ' '):
            print("Pause")
            time.sleep(PAUSE_DURATION)
        else:
            print("Writing", note)
            # Robot.set_value(SOUND, "PITCH", MAP[note])
            time.sleep(NOTE_DURATION)

def autonomous_main():
    pass

#################################### TELEOP ####################################

def teleop_setup():
    print("Now executing TELEOP SETUP")
    # Robot.run(print_button)
    # Robot.run(play_notes)
    pass

def teleop_main():
    if Gamepad.get_value('button_a'):
        print("BUTTON A IS PRESSED")
    if Gamepad.get_value('button_b'):
        print("BUTTON B IS PRESSED")

################################### THREADS ####################################

def print_button():
    while (1):
        if Gamepad.get_value('button_a'):
            print("BUTTON A IS PRESSED")
        if Gamepad.get_value('button_b'):
            print("BUTTON B IS PRESSED")

def play_notes():
    while (1):
        if Gamepad.get_value('button_a'):
            Robot.set_value(SOUND, "PITCH", MAP['A'])
        if Gamepad.get_value('button_b'):
            Robot.set_value(SOUND, "PITCH", MAP['B'])
