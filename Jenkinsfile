def getRevision(b) {
    def gitBuildData = b.getAction(hudson.plugins.git.util.BuildData)
    if (gitBuildData != null) {
        return gitBuildData.lastBuiltRevision
    }
    def svnRevisionState = b.getAction(hudson.scm.SubversionTagAction)
    if (svnRevisionState != null) {
        return svnRevisionState.tags.keySet()[0].revision
    }
    return null
}

def getPaMap(ParametersAction pa) {
    def m = [:]
    if (pa != null) {
        for (p in pa.getParameters()) {
           m[p.name] = p.value
        }
    }
    return m
}

def getCurrentRevision() {
    return getRevision(currentBuild.rawBuild)
}

def getPreviousRevision() {
    def curBuild = currentBuild.rawBuild
    def curPaMap = getPaMap(curBuild.getAction(ParametersAction.class))
    
    def b = curBuild.parent.lastSuccessfulBuild
    while (b != null) {
        def paMap = getPaMap(b.getAction(ParametersAction.class))
        if (paMap == curPaMap) {
            return getRevision(b)
        }
        
        b = b.previousSuccessfulBuild
    }
    
    return null
}

stage ('Set properties') {
    properties([
        disableConcurrentBuilds(),
        parameters([
            booleanParam(name: 'wipe', defaultValue: false, description: 'wipe workspace before build (clear build)')
        ])
    ])
}

node {
    stage ("Cleanup") {
        sh "rm -rf jenkins/out"
        if (wipe.equals('true')) {
            sh "rm -fr `ls -A`"
        }
    }

    stage ("Checkout") {
        checkout scm
    }
    
    stage ("Check changes") {
        if (getCurrentRevision() == getPreviousRevision()) {
            currentBuild.result = "ABORTED"
            println "! No changes, abort build"
        }
    }
    
    if (currentBuild.result == null) {
        stage ("Build") {
            sh "./docker/build.sh"
        }
        
        stage ("Save artifacts") {
            def archive_dir = currentBuild.rawBuild.pickArtifactManager().root().toURI().path
            sh "mkdir ${archive_dir}"
            sh "tar cf - jenkins/out/linux-* | tar xf - -C ${archive_dir}"
        }
    }
}
