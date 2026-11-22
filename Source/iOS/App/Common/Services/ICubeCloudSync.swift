import PVCloudSync

/// Persistent identity of the existing iCube private CloudKit database.
enum ICubeCloudSync {
    static let configuration = CloudSyncConfiguration(
        containerIdentifier: "iCloud.com.strikekaboom.primehack",  // PrimeHack for iPad: own container, separate from iCube
        zoneName: "iCubeUserData",
        recordType: "iCubeUserFile",
        containerIsProvisioned: true
    )
}
