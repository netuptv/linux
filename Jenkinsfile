catchError {
    stage('Configure build') {
        properties([
            disableConcurrentBuilds(),
            buildDiscarder(logRotator(
                artifactDaysToKeepStr: '',
                artifactNumToKeepStr: '',
                daysToKeepStr: '',
                numToKeepStr: '50')),
            parameters([
                booleanParam(description: 'Wipe out workspace before build', name: 'wipe')]),
            pipelineTriggers([
                pollSCM('H/5 * * * *')
            ])
        ])
    }

    node() {
        stage('Clean up') {
            if (params['wipe']) {
                deleteDir()
            } else {
                dir('jenkins/out') { 
                    deleteDir() 
                }
            } 
        }

        stage('Checkout') {
            checkout scm
        }

        stage('Build') {
            dir('jenkins') {
                sh "../docker/build.sh"
            }
        }

        stage('Save artifacts') {
            archiveArtifacts 'jenkins/out/kernel/**'
        }
    }
}

node() {
    step([
        $class: 'Mailer',
        notifyEveryUnstableBuild: true,
        recipients: 'kvi@netup.ru',
        sendToIndividuals: false
    ])
}
