#!/usr/bin/env python
# -*- coding: utf-8 -*-

# Proof-of-Concept for connecting the Alma library platform from Ex Libris
# to a RFID reader built using an Arduino and a BM019 NFC/RFID module.
# Sets up a web server on the local machine that communicaties with Alma running in a web browser
# and with the Arduino over a serial interface.
#
# Intended as a prototype and for inspiration, please
# DO NOT USE IN PRODUCTION without a serious code review.

# Bottle is used for HTTP
# https://bottlepy.org

# Importing Libraries
from bottle import get, post, route, run, response, request
from io import TextIOWrapper
from xml.etree.ElementTree import fromstring, ElementTree
import json
import serial 
import time

# This is where the Arduino listens, change it to your COM port
arduino_port = 'COM10'

# This is where the HTTP server listens, change it as you prefer
host = 'localhost'
port = 8000

arduino = serial.Serial(port=arduino_port, baudrate=9600, timeout=5)

# Send commands to Arduino
# The following commands can be used:
# '1' - Get the barcode/item identifier
# '2' - Get the scurity status
# '3' - Turn security on
# '4' - Turn security off
# '5' - Write new barcode/item identifier to the tag - the barcode should follow the 5, E.G. '51234'
# '9' - Stop scanning for tags
def write_to_arduino(command):
    arduino.reset_input_buffer()
    arduino.write(bytes(command, 'utf-8'))

# Collect data from Arduino
# The format of the answer looks like:
# {"r":"", "e":"", "s":"", "p":"", "c1":"", "c2":""}
#   r - result - can be '0' or '1', '1' is success
#   e - error - contains an error message if result is '0'
#   s - security - can be '0' or '1', '1' is on
#   p - payload - if command is '1' this is the barcode
#   c1 - CRC recorded - CRC recorded in the RF tag 
#   c2 - CRC calculated - CRC calulated from other values in the RF tag
def collect_data_from_arduino():
    retries = 0
    max_retries = 2
    time.sleep(0.5)
    data = arduino.readline()
    while not data and retries < max_retries:
        retries = retries + 1
        time.sleep(0.5)
        data = arduino.readline()
    if not data:
        arduino.close()
        time.sleep(2)
        arduino.open()
        raise Exception("Error reading RFID response, resetting connection") 
    return data

# Escape XML characters
def xml_special_chars(string):
    string = string.replace("&", "&amp;")
    string = string.replace("<", "&lt;")
    string = string.replace(">", "&gt;")
    string = string.replace("\"", "&quot;")
    string = string.replace("'", "&apos;")
    return string

def set_headers():
    response.set_header('Access-Control-Allow-Origin', '*')
    response.set_header('Access-Control-Allow-Methods', 'POST, GET, OPTIONS')
    response.set_header('Access-Control-Allow-Headers', 'X-Requested-With, Content-Type')
    response.set_header('Content-Type', 'application/xml')
    

# GET request method
@route('/getItems', method='GET')
def get_items():
    
    output_barcode = ''
    security = ''
    rfid_response_ok = True
    error_message = ''
    xml = ''

    set_headers()
    
    try:
        write_to_arduino('1')
        rfid_response = collect_data_from_arduino()
        rfid_response = rfid_response.decode()
    except:
        rfid_response_ok = False
        error_message = error_message + 'Problems getting response from RFID reader'

    if(rfid_response_ok):
        try:
            # A reponse can look like: '{"r":"1", "e":"", "s":"1", "p":"11240000237988", "c1":"XXXX", "c2":"XXXX"}'
            json_object = json.loads(rfid_response)
            if(json_object["r"] == '0'):
                rfid_response_ok = False
                error_message = error_message + rfid_response["e"]
            else:
                output_barcode = json_object["p"]
                if(json_object["s"] == '1'):
                    security = 'on'
                else:
                    security = 'off'
        except:
            rfid_response_ok = False
            error_message = error_message + 'Problems decoding response from RFID reader'

    if(not rfid_response_ok):
        xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
                    <rfid><ExceptionDetail><Message>"""
        xml = xml + xml_special_chars(error_message)
        xml = xml + "</Message></ExceptionDetail></rfid>"
        return(xml)
    else:
        # Alma doesn't do anything with the material_type, library, location. The
        # most important is the barcode. is_complete - when false Alma displays a
        # warning.
        # The RFID POC has only implemented reading one tag at a time
        xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
                    <rfid>
                        <items>
                        <item>
                          <barcode>"""
        xml = xml + xml_special_chars(output_barcode)
        xml = xml + """</barcode>
                          <is_secure>"""
        if(security == "on"):
            xml = xml + "true"
        else:
            xml = xml + "false"
        xml = xml + """</is_secure>
                          <!-- false = it has been checked out -->
                          <is_complete>true</is_complete>
                          <!-- for 1 part item it will always be true -->
                          <total_num_of_parts>1</total_num_of_parts>
                          <tags>
                            <tag>
                              <!-- a 'real' tag id do exist but here we use the barcode -->
                              <tag_id>"""
        xml = xml + xml_special_chars(output_barcode)
        xml = xml + """</tag_id>
                              <part_num>1</part_num>
                              <material_type> </material_type> <!-- must exist even if it is not used -->
                              <library> </library> <!-- must exist even if it is not used -->
                              <location> </location> <!-- must exist even if it is not used -->
                            </tag>
                          </tags>
                        </item>
                      </items>
                    </rfid>"""
        return(xml)
		

# POST request method for updating barcode
@route('/itemUpdate', method='POST')
def post_update():

    rfid_response_ok = True
    error_message = ''
    xml = ''

    set_headers()

    try:
        with TextIOWrapper(request.body, encoding = "UTF-8") as lines:
            postdata = ''.join(lines.readlines())
        tree = ElementTree(fromstring(postdata))
        root = tree.getroot()
        items_node = root.find("items")
        item_node = items_node.find("item")
        barcode = item_node.find("barcode").text
    except:
        rfid_response_ok = False
        error_message = error_message + 'Problems decoding Item Update command from Alma'

    if(rfid_response_ok):
        try:
            write_to_arduino('5' + barcode)
            rfid_response = collect_data_from_arduino()
            rfid_response = rfid_response.decode()
        except:
            rfid_response_ok = False
            error_message = error_message + 'Problems getting response from RFID reader'

    if(rfid_response_ok):
        try:
            json_object = json.loads(rfid_response)
            if(json_object["r"] == '0'):
                rfid_response_ok = False
                error_message = rfid_response["e"]
        except:
            rfid_response_ok = False
            error_message = error_message + 'Problems decoding response from RFID reader'

    if(not rfid_response_ok):
        xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
                    <rfid><ExceptionDetail><Message>"""
        xml = xml + xml_special_chars(error_message)
        xml = xml + "</Message></ExceptionDetail></rfid>"
        return(xml)
    else:
        xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
                    <rfid>
                       <success>true</success>
                    </rfid>"""
        return(xml)


# POST request method for security
@route('/setSecurity', method='POST')
def post_security():

    rfid_response_ok = True
    error_message = ''
    xml = ''

    set_headers()
    
    try:
        with TextIOWrapper(request.body, encoding = "UTF-8") as lines:
            postdata = ''.join(lines.readlines())
        tree = ElementTree(fromstring(postdata))
        root = tree.getroot()
        is_secure = root.find("is_secure").text
    except:
        rfid_response_ok = False
        error_message = error_message + 'Problems decoding Set Security command from Alma'

    if(rfid_response_ok):
        try:
            if(is_secure=='true'):
                write_to_arduino('3')
            else:
                write_to_arduino('4')
            rfid_response = collect_data_from_arduino()
            rfid_response = rfid_response.decode()
        except:
            rfid_response_ok = False
            error_message = error_message + 'Problems getting response from RFID reader'

    if(rfid_response_ok):
        try:
            json_object = json.loads(rfid_response)
            if(json_object["r"] == '0'):
                rfid_response_ok = False
                error_message = rfid_response["e"]
        except:
            rfid_response_ok = False
            error_message = error_message + 'Problems decoding response from RFID reader'

    if(not rfid_response_ok):
        xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
                    <rfid><ExceptionDetail><Message>"""
        xml = xml + xml_special_chars(error_message)
        xml = xml + "</Message></ExceptionDetail></rfid>"
        return(xml)
    else:
        xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
                    <rfid>
                       <success>true</success>
                    </rfid>"""
        return(xml)


# Any other request method including OPTIONS which is used by Alma
@route('/getItems', method='ANY')
def get_items_any():
    set_headers()
    return ''
	
# Any other request method including OPTIONS which is used by Alma
@route('/setSecurity', method='ANY')
def set_security_any():
    set_headers()
    return ''

# Any other request method including OPTIONS which is used by Alma
@route('/itemUpdate', method='ANY')
def item_update_any():
    set_headers()
    return ''

if __name__ == '__main__':
    run(host=host, port=port, debug=True)

# Run the command:
# py alma_bm019_rfid_proxy.py
# Access on:
# http://localhost:8000

# https://developers.exlibrisgroup.com/alma/integrations/rfid/information_for_rfid_vendor/
# https://bottlepy.org/docs/dev/
