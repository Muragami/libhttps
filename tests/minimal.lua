--[[
	a minimal lua libhttps example
]]

-- open the library
https = require 'libhttps'

print ("- minimal libhttps example")

-- initialize it with the default 16kb buffer size
--		note: we could pass an integer here with the number of buffer bytes
https.init()

print ("- libhttps initialized")

-- setup out callbacks just to show what happens to a request
callbacks = {
	handle = {},	-- if we create this table, it'll be populated with handles created by requests

	start = function(self, hid, url, msg, code, sz, data)
		print ("\t" .. url .. " has started [" .. hid .. "]")
	end,

	update = function(self, hid, url, msg, code, sz, data)
		print ("\t" .. url .. " has updated code: " .. code .. " [" .. hid .. "]")
	end,

	headers = function(self, hid, url, msg, code, sz, data)
		print ("\t" .. url .. " all headers read [" .. hid .. "]")
	end,

	length = function(self, hid, url, msg, len, sz, data)
		print ("\t" .. url .. " content length is " .. len .. " bytes [" .. hid .. "]")
	end,

	mime = function(self, hid, url, msg, code, sz, mime)
		print ("\t" .. url .. " mime type is '" .. mime .. "'' [" .. hid .. "]")
	end,

	read = function(self, hid, url, msg, bytes, sz, data)
		print ("\t" .. url .. " read " .. bytes .. " bytes [" .. hid .. "]")
	end,

	complete = function(self, hid, url, msg, code, sz, data)
		print ("\t" .. url .. " is complete.")
		-- let the system know it can release this request object back into the pool, as we are done with it
		https.release(hid)
		self.handle[hid] = nil
		working = false
	end,
}

-- work work work, all day long
working = true

-- make the update call sleep a bit each time
--		note: makes https.update() sleep 0.025 seconds each call
https.options("EASY_OPT_DELAY", 0.025);

print ("- libhttps configured")

-- make the request
--		note: we could pass headers too like: https.get(url, callbacks, headers)
rHandle = https.get("https://www.lua.org/manual/5.1/index.html", callbacks)
print ("- issuing GET request for https://www.lua.org/manual/5.1/index.html")

-- loop until we are complete, each call to .update() will issue callbacks from the async request
while (working) do
	https.update()
end

-- do that again, but this time we will explore the headers returned
function callbacks:headers(handle, url, msg, code, sz, data)
	print ("\t" .. url .. " all headers read [" .. handle .. "]")
	-- https.list() iterates over all returned headers and place them into a new table
	headerTable = https.list(handle)
	for k, v in pairs(headerTable) do
    	print("\t\t" .. k .. ": " .. v)
	end
end

working = true
rHandle = https.get("https://wiki.wishray.com", callbacks)
print ("- issuing GET request for https://wiki.wishray.com")

-- loop until we are complete, each call to .update() will issue callbacks from the async request
while (working) do
	https.update()
end

-- do that again, but this time we will also print out the body
function callbacks:complete(hid, url, msg, code, sz, read)
		print ("\t" .. url .. " is complete, " .. sz .. " bytes total.")
		-- get the body as a lua string and dump it out
		print (https.body(hid))
		-- all done
		https.release(hid)
		self.handle[hid] = nil
		working = false		
end

working = true
rHandle = https.get("https://wishray.com", callbacks)
print ("- issuing GET request for https://wishray.com")

-- loop until we are complete, each call to .update() will issue callbacks from the async request
while (working) do
	https.update()
end


