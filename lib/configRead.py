import json

def load(file = 'config.json'):
  global cnf

  try:
    fd = open(file, 'r')
    cnf = json.load(fd)

  except ValueError as e:
    print "Malformed line in file: '"+file+"': " + str(e)
    exit(1)

  except:
    print "Failed to load configuration file '" + file + "'. Aborting."
    raise



