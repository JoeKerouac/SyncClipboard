package com.syncclipboard.model;

import java.util.List;

public class Message {
    private String type;
    private String serverKey;
    private String username;
    private String password;
    private String content;
    private String deviceId;

    // file transfer fields
    private String fileId;
    private String fileName;
    private String mimeType;
    private long fileSize;
    private String checksum;
    private List<String> localAddresses;
    private String data;         // base64 file content for relay
    private String targetDevice;
    private String method;       // "lan" / "nat" / "relay" / "failed"
    private boolean success;
    private int fileTransferLevel;
    private long connectionMs;
    private long transferMs;

    public String getType() { return type; }
    public void setType(String type) { this.type = type; }
    public String getServerKey() { return serverKey; }
    public void setServerKey(String serverKey) { this.serverKey = serverKey; }
    public String getUsername() { return username; }
    public void setUsername(String username) { this.username = username; }
    public String getPassword() { return password; }
    public void setPassword(String password) { this.password = password; }
    public String getContent() { return content; }
    public void setContent(String content) { this.content = content; }
    public String getDeviceId() { return deviceId; }
    public void setDeviceId(String deviceId) { this.deviceId = deviceId; }

    public String getFileId() { return fileId; }
    public void setFileId(String fileId) { this.fileId = fileId; }
    public String getFileName() { return fileName; }
    public void setFileName(String fileName) { this.fileName = fileName; }
    public String getMimeType() { return mimeType; }
    public void setMimeType(String mimeType) { this.mimeType = mimeType; }
    public long getFileSize() { return fileSize; }
    public void setFileSize(long fileSize) { this.fileSize = fileSize; }
    public String getChecksum() { return checksum; }
    public void setChecksum(String checksum) { this.checksum = checksum; }
    public List<String> getLocalAddresses() { return localAddresses; }
    public void setLocalAddresses(List<String> localAddresses) { this.localAddresses = localAddresses; }
    public String getData() { return data; }
    public void setData(String data) { this.data = data; }
    public String getTargetDevice() { return targetDevice; }
    public void setTargetDevice(String targetDevice) { this.targetDevice = targetDevice; }
    public String getMethod() { return method; }
    public void setMethod(String method) { this.method = method; }
    public boolean isSuccess() { return success; }
    public void setSuccess(boolean success) { this.success = success; }
    public int getFileTransferLevel() { return fileTransferLevel; }
    public void setFileTransferLevel(int fileTransferLevel) { this.fileTransferLevel = fileTransferLevel; }
    public long getConnectionMs() { return connectionMs; }
    public void setConnectionMs(long connectionMs) { this.connectionMs = connectionMs; }
    public long getTransferMs() { return transferMs; }
    public void setTransferMs(long transferMs) { this.transferMs = transferMs; }
}
